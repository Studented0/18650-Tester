/*
 * CellScan — 4-Channel 18650 Battery Analyzer
 * ESP32 + TCA9548A + 4x INA219 + IRLZ44N MOSFETs + SSD1306 OLED
 *
 * GPIO:
 *   SDA=21, SCL=22
 *   MOSFET gates: CH0=25, CH1=26, CH2=27, CH3=14
 *
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>

// ── WiFi credentials ─────────────────────────────────────
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

// ── Hardware config ──────────────────────────────────────
#define TCA_ADDR        0x70
#define INA219_ADDR     0x40
#define NUM_CHANNELS    4
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_RESET      -1

const int MOSFET_PINS[NUM_CHANNELS] = {25, 26, 27, 14};

// ── Test config ──────────────────────────────────────────
#define CUTOFF_VOLTAGE_V   2.80f
#define POLL_INTERVAL_MS   500
#define DCIR_SETTLE_MS     100

// ── Channel state machine ─────────────────────────────────
enum ChannelState {
  CH_IDLE,
  CH_MEASURING_DCIR,
  CH_DISCHARGING,
  CH_DONE,
  CH_ERROR
};

struct ChannelData {
  ChannelState  state           = CH_IDLE;
  float         voltage_v       = 0.0f;
  float         current_ma      = 0.0f;
  float         capacity_mah    = 0.0f;
  float         resistance_mohm = 0.0f;
  float         voc             = 0.0f;
  unsigned long start_ms        = 0;
  unsigned long last_ms         = 0;
  float         cutoff_v        = CUTOFF_VOLTAGE_V;
  bool          cell_present    = false;
};

ChannelData      channels[NUM_CHANNELS];
Adafruit_INA219  ina219(INA219_ADDR);
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
WebServer        server(80);

int display_channel = 0;

// ── TCA9548A mux ─────────────────────────────────────────
void tca_select(uint8_t ch) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
}

// ── INA219 read ───────────────────────────────────────────
bool ina_read(uint8_t ch, float &v, float &i) {
  tca_select(ch);
  delayMicroseconds(100);
  float bus_v    = ina219.getBusVoltage_V();
  float shunt_mv = ina219.getShuntVoltage_mV();
  if (bus_v < 0.1f && shunt_mv == 0.0f) {
    v = 0.0f; i = 0.0f;
    return false;
  }
  v = bus_v + (shunt_mv / 1000.0f);
  i = ina219.getCurrent_mA();
  return true;
}

// ── MOSFET control ────────────────────────────────────────
void mosfet_on(uint8_t ch)  { digitalWrite(MOSFET_PINS[ch], HIGH); }
void mosfet_off(uint8_t ch) { digitalWrite(MOSFET_PINS[ch], LOW);  }

// ── DCIR measurement ──────────────────────────────────────
void measure_dcir(uint8_t ch) {
  float v_oc, i_oc, v_load, i_load;
  mosfet_off(ch);
  delay(50);
  ina_read(ch, v_oc, i_oc);
  channels[ch].voc = v_oc;
  mosfet_on(ch);
  delay(DCIR_SETTLE_MS);
  ina_read(ch, v_load, i_load);
  if (i_load > 1.0f) {
    channels[ch].resistance_mohm = ((v_oc - v_load) / (i_load / 1000.0f)) * 1000.0f;
  } else {
    channels[ch].resistance_mohm = 0.0f;
  }
}

// ── Start / stop test ─────────────────────────────────────
void start_test(uint8_t ch) {
  channels[ch].capacity_mah = 0.0f;
  channels[ch].state        = CH_MEASURING_DCIR;
  channels[ch].start_ms     = millis();
  channels[ch].last_ms      = millis();
  measure_dcir(ch);
  channels[ch].state = CH_DISCHARGING;
  mosfet_on(ch);
  Serial.printf("[CH%d] Started. VOC=%.3fV DCIR=%.1fmOhm\n",
                ch, channels[ch].voc, channels[ch].resistance_mohm);
}

void stop_test(uint8_t ch) {
  mosfet_off(ch);
  if (channels[ch].state == CH_DISCHARGING) {
    channels[ch].state = CH_DONE;
    Serial.printf("[CH%d] Done. %.2fmAh %.1fmOhm\n",
                  ch, channels[ch].capacity_mah, channels[ch].resistance_mohm);
  } else {
    channels[ch].state = CH_IDLE;
  }
}

// ── Coulomb counting poll ─────────────────────────────────
void poll_channel(uint8_t ch) {
  float v, i;
  channels[ch].cell_present = ina_read(ch, v, i);
  channels[ch].voltage_v    = v;
  channels[ch].current_ma   = i;
  if (channels[ch].state == CH_DISCHARGING) {
    unsigned long now   = millis();
    unsigned long delta = now - channels[ch].last_ms;
    channels[ch].last_ms = now;
    if (i > 0) {
      channels[ch].capacity_mah += (i / 1000.0f) * (delta / 3600000.0f) * 1000.0f;
    }
    if (v < channels[ch].cutoff_v || v < 0.5f) {
      stop_test(ch);
    }
  }
}

// ── OLED ─────────────────────────────────────────────────
const char* state_str(ChannelState s) {
  switch (s) {
    case CH_IDLE:           return "IDLE";
    case CH_MEASURING_DCIR: return "DCIR";
    case CH_DISCHARGING:    return "DSG ";
    case CH_DONE:           return "DONE";
    case CH_ERROR:          return "ERR ";
    default:                return "?   ";
  }
}

void update_display() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("CellScan  CH%d/%d", display_channel + 1, NUM_CHANNELS);
  oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  ChannelData &ch = channels[display_channel];
  oled.setCursor(0, 13);
  oled.printf("[%s] %.3fV", state_str(ch.state), ch.voltage_v);
  oled.setCursor(0, 23);
  oled.printf("I: %.1f mA", ch.current_ma);
  oled.setCursor(0, 33);
  oled.printf("Cap: %.1f mAh", ch.capacity_mah);
  oled.setCursor(0, 43);
  oled.printf("ESR: %.0f mOhm", ch.resistance_mohm);
  oled.setCursor(0, 55);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP().toString());
  } else {
    oled.print("WiFi connecting..");
  }
  oled.display();
}

// ── Web server ────────────────────────────────────────────
void handle_status() {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.createNestedArray("channels");
  for (int i = 0; i < NUM_CHANNELS; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["channel"]         = i;
    obj["state"]           = state_str(channels[i].state);
    obj["voltage_v"]       = String(channels[i].voltage_v,       3);
    obj["current_ma"]      = String(channels[i].current_ma,      1);
    obj["capacity_mah"]    = String(channels[i].capacity_mah,    2);
    obj["resistance_mohm"] = String(channels[i].resistance_mohm, 1);
    obj["cell_present"]    = channels[i].cell_present;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handle_start() {
  if (!server.hasArg("ch")) { server.send(400, "text/plain", "missing ch"); return; }
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch >= NUM_CHANNELS) { server.send(400, "text/plain", "invalid ch"); return; }
  if (channels[ch].state == CH_DISCHARGING) { server.send(409, "text/plain", "already running"); return; }
  start_test(ch);
  server.send(200, "text/plain", "ok");
}

void handle_stop() {
  if (!server.hasArg("ch")) { server.send(400, "text/plain", "missing ch"); return; }
  int ch = server.arg("ch").toInt();
  if (ch < 0 || ch >= NUM_CHANNELS) { server.send(400, "text/plain", "invalid ch"); return; }
  stop_test(ch);
  server.send(200, "text/plain", "ok");
}

void handle_root() {
  String h = "";
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>CellScan</title><style>";
  h += "body{font-family:monospace;background:#111;color:#eee;padding:20px}";
  h += "h1{color:#0f0}";
  h += "table{border-collapse:collapse;width:100%;margin:16px 0}";
  h += "th,td{border:1px solid #333;padding:8px 12px;text-align:center}";
  h += "th{background:#222;color:#0f0}";
  h += ".idle{color:#888}.dcir{color:#ff0}.dsg{color:#0ff}.done{color:#0f0}.err{color:#f00}";
  h += "button{margin:4px;padding:6px 14px;font-family:monospace;cursor:pointer;";
  h += "background:#222;color:#eee;border:1px solid #555;border-radius:4px}";
  h += "button:hover{background:#333}";
  h += "</style></head><body><h1>CellScan</h1>";
  h += "<table id='t'><tr><th>CH</th><th>State</th><th>Voltage</th>";
  h += "<th>Current</th><th>Capacity</th><th>ESR</th><th>Action</th></tr></table>";
  h += "<script>";
  h += "async function fetchStatus(){";
  h += "var r=await fetch('/api/status');";
  h += "var d=await r.json();";
  h += "var t=document.getElementById('t');";
  h += "while(t.rows.length>1)t.deleteRow(1);";
  h += "d.channels.forEach(function(ch){";
  h += "var s=ch.state.trim().toLowerCase();";
  h += "var c=s==='idle'?'idle':s==='dcir'?'dcir':s==='dsg'?'dsg':s==='done'?'done':'err';";
  h += "var b=(s==='idle'||s==='done')";
  h += "?'<button onclick=\"startTest('+ch.channel+')\">Start</button>'";
  h += ":'<button onclick=\"stopTest('+ch.channel+')\">Stop</button>';";
  h += "t.innerHTML+='<tr><td>'+(ch.channel+1)+'</td>'";
  h += "+'<td class=\"'+c+'\">'+ch.state+'</td>'";
  h += "+'<td>'+ch.voltage_v+' V</td>'";
  h += "+'<td>'+ch.current_ma+' mA</td>'";
  h += "+'<td>'+ch.capacity_mah+' mAh</td>'";
  h += "+'<td>'+ch.resistance_mohm+' mOhm</td>'";
  h += "+'<td>'+b+'</td></tr>';";
  h += "});";
  h += "}";
  h += "async function startTest(c){await fetch('/api/start?ch='+c,{method:'POST'});}";
  h += "async function stopTest(c){await fetch('/api/stop?ch='+c,{method:'POST'});}";
  h += "fetchStatus();setInterval(fetchStatus,1000);";
  h += "</script></body></html>";
  server.send(200, "text/html", h);
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(MOSFET_PINS[i], OUTPUT);
    mosfet_off(i);
  }

  Wire.begin(21, 22);

  if (!ina219.begin()) {
    Serial.println("INA219 not found");
  }
  ina219.setCalibration_32V_2A();

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print("CellScan starting...");
  oled.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed - running offline");
  }

  server.on("/",           HTTP_GET,  handle_root);
  server.on("/api/status", HTTP_GET,  handle_status);
  server.on("/api/start",  HTTP_POST, handle_start);
  server.on("/api/stop",   HTTP_POST, handle_stop);
  server.begin();

  Serial.println("CellScan ready.");
}

// ── Loop ──────────────────────────────────────────────────
unsigned long last_poll = 0;
unsigned long last_disp = 0;

void loop() {
  server.handleClient();
  unsigned long now = millis();
  if (now - last_poll >= POLL_INTERVAL_MS) {
    last_poll = now;
    for (int i = 0; i < NUM_CHANNELS; i++) {
      poll_channel(i);
    }
    if (now - last_disp >= 3000) {
      last_disp = now;
      display_channel = (display_channel + 1) % NUM_CHANNELS;
    }
    update_display();
  }
}
