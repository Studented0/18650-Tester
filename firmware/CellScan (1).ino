/*
 * CellScan — 4-Channel 18650 Battery Analyzer
 * ESP32 + TCA9548A + 4x INA219 + IRLZ44N + 4x TP4056 + EC11 + SSD1306
 *
 * Pin map (verified against CellScan_Schematic.kicad_sch):
 *   SDA       = GPIO21   SCL       = GPIO22
 *   GATE_CH1  = GPIO25   GATE_CH2  = GPIO26
 *   GATE_CH3  = GPIO27   GATE_CH4  = GPIO32
 *   CE_CH1    = GPIO13   CE_CH2    = GPIO12  (see boot note below)
 *   CE_CH3    = GPIO14   CE_CH4    = GPIO33
 *   ENC_A     = GPIO15   ENC_B     = GPIO2   ENC_SW = GPIO5
 *
 * HARDWARE NOTE — GPIO12 (CE_CH2):
 *   GPIO12 is a strapping pin. High at boot selects 1.8V flash mode,
 *   which causes boot failures on standard 3.3V flash modules.
 *   CE_CH2 has a 10k pull-up to +5V, so GPIO12 sees ~5V through 10k at boot.
 *   If the board won't boot, change the CE_CH2 pull-up resistor from +5V to +3V3.
 *
 * ENCODER NOTES:
 *   ENC_B = GPIO2. Pull-up to +3V3 = HIGH at boot = fine (normal boot).
 *   GPIO2 may drive the onboard LED on some dev boards — expect flicker.
 *   ENC_A = GPIO15. Pull-up to +3V3 = HIGH at boot = extra bootloader
 *   UART output, but doesn't break boot.
 *
 * ENCODER UI:
 *   Rotate       = navigate channels / menu items / adjust values
 *   Short press  = select / confirm
 *   Long press   = back / cancel
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>

const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

#define TCA_ADDR     0x70
#define INA219_ADDR  0x40
#define NUM_CH       4
#define OLED_W       128
#define OLED_H       64
#define OLED_RST     -1

const int MOSFET_PINS[NUM_CH] = {25, 26, 27, 32};
const int CE_PINS[NUM_CH]     = {13, 12, 14, 33};

#define ENC_A   15
#define ENC_B   2
#define ENC_SW  5

#define CUTOFF_DEF   2.80f
#define CUTOFF_MIN   2.50f
#define CUTOFF_MAX   3.20f
#define CUTOFF_STEP  0.05f
#define POLL_MS      500
#define DCIR_MS      100

// ---------- Channel state ----------
enum ChanState { ST_IDLE, ST_DCIR, ST_DSG, ST_DONE, ST_ERR };

struct Channel {
  ChanState state     = ST_IDLE;
  float     v         = 0;
  float     i_ma      = 0;
  float     mah       = 0;
  float     esr       = 0;
  float     voc       = 0;
  float     cutoff    = CUTOFF_DEF;
  uint32_t  last_ms   = 0;
};

Channel ch[NUM_CH];

// ---------- UI state ----------
enum Screen { SCR_CHAN, SCR_MENU, SCR_CUTOFF };

Screen  scr       = SCR_CHAN;
int     sel_ch    = 0;
int     sel_item  = 0;
float   edit_val  = 0;

const char* ITEMS[] = {"Start Test", "Stop Test", "Set Cutoff", "Back"};
const int   N_ITEMS = 4;

// ---------- Encoder ----------
volatile int enc_delta = 0;
void IRAM_ATTR enc_isr() {
  if (digitalRead(ENC_B)) enc_delta++;
  else                     enc_delta--;
}

uint32_t btn_ms   = 0;
bool     btn_down = false;
bool     ev_sp    = false;
bool     ev_lp    = false;

void poll_btn() {
  bool d = (digitalRead(ENC_SW) == LOW);
  if (d && !btn_down) { btn_ms = millis(); btn_down = true; }
  else if (!d && btn_down) {
    btn_down = false;
    if (millis() - btn_ms >= 500) ev_lp = true;
    else                          ev_sp = true;
  }
}

// ---------- Peripherals ----------
Adafruit_INA219  ina(INA219_ADDR);
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RST);
WebServer        srv(80);

// ---------- Hardware helpers ----------
void tca(uint8_t c) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << c);
  Wire.endTransmission();
}

void gate_on(int c)  { digitalWrite(MOSFET_PINS[c], HIGH); }
void gate_off(int c) { digitalWrite(MOSFET_PINS[c], LOW);  }

void ce_off(int c) { pinMode(CE_PINS[c], OUTPUT); digitalWrite(CE_PINS[c], LOW); }
void ce_on(int c)  { pinMode(CE_PINS[c], INPUT); }  // pull-up brings CE high

bool ina_read(int c, float &v, float &i_ma) {
  tca(c);
  delayMicroseconds(200);
  float bv = ina.getBusVoltage_V();
  float sv = ina.getShuntVoltage_mV();
  v    = bv + sv / 1000.0f;
  i_ma = ina.getCurrent_mA();
  return !(v < 0.5f && fabsf(i_ma) < 1.0f);
}

const char* st_str(ChanState s) {
  switch (s) {
    case ST_IDLE: return "IDLE"; case ST_DCIR: return "DCIR";
    case ST_DSG:  return "DSG "; case ST_DONE: return "DONE";
    case ST_ERR:  return "ERR "; default:       return "????";
  }
}

// ---------- Test control ----------
void start_test(int c) {
  float v, i_ma;
  if (!ina_read(c, v, i_ma) || v < 2.5f) { ch[c].state = ST_ERR; return; }
  ce_off(c);
  delay(200);
  ch[c].state = ST_DCIR;
  ina_read(c, ch[c].voc, i_ma);
  gate_on(c);
  delay(DCIR_MS);
  float vl, il;
  ina_read(c, vl, il);
  if (il > 1.0f) ch[c].esr = ((ch[c].voc - vl) / (il / 1000.0f)) * 1000.0f;
  ch[c].state   = ST_DSG;
  ch[c].mah     = 0;
  ch[c].last_ms = millis();
}

void stop_test(int c) {
  gate_off(c);
  ce_on(c);
  ch[c].state = ST_IDLE;
}

void update_ch(int c) {
  if (ch[c].state != ST_DSG) return;
  uint32_t now = millis();
  if (now - ch[c].last_ms < POLL_MS) return;
  float v, i_ma;
  if (!ina_read(c, v, i_ma)) { stop_test(c); ch[c].state = ST_ERR; return; }
  ch[c].v    = v;
  ch[c].i_ma = i_ma;
  ch[c].mah += i_ma * (float)(now - ch[c].last_ms) / 3600000.0f;
  ch[c].last_ms = now;
  if (v <= ch[c].cutoff) { gate_off(c); ce_on(c); ch[c].state = ST_DONE; }
}

// ---------- UI logic ----------
void menu_act(int item) {
  switch (item) {
    case 0:
      if (ch[sel_ch].state == ST_IDLE || ch[sel_ch].state == ST_DONE)
        start_test(sel_ch);
      scr = SCR_CHAN; break;
    case 1: stop_test(sel_ch); scr = SCR_CHAN; break;
    case 2: edit_val = ch[sel_ch].cutoff; scr = SCR_CUTOFF; break;
    case 3: scr = SCR_CHAN; break;
  }
}

void handle_ui() {
  poll_btn();
  int d  = enc_delta; enc_delta = 0;
  bool sp = ev_sp; ev_sp = false;
  bool lp = ev_lp; ev_lp = false;

  switch (scr) {
    case SCR_CHAN:
      if (d)  sel_ch = (sel_ch + d + NUM_CH) % NUM_CH;
      if (sp) { sel_item = 0; scr = SCR_MENU; }
      break;
    case SCR_MENU:
      if (d)  sel_item = (sel_item + d + N_ITEMS) % N_ITEMS;
      if (sp) menu_act(sel_item);
      if (lp) scr = SCR_CHAN;
      break;
    case SCR_CUTOFF:
      if (d)  edit_val = constrain(edit_val + d * CUTOFF_STEP, CUTOFF_MIN, CUTOFF_MAX);
      if (sp) { ch[sel_ch].cutoff = edit_val; scr = SCR_MENU; }
      if (lp) scr = SCR_MENU;
      break;
  }
}

// ---------- OLED ----------
void draw_chan() {
  Channel &c = ch[sel_ch];
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("CELL%d  [%s]", sel_ch + 1, st_str(c.state));
  oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  oled.setCursor(0, 12); oled.printf("V:   %.3f V",   c.v);
  oled.setCursor(0, 21); oled.printf("I:   %.0f mA",  c.i_ma);
  oled.setCursor(0, 30); oled.printf("Cap: %.1f mAh", c.mah);
  oled.setCursor(0, 39); oled.printf("ESR: %.0f mO",  c.esr);
  oled.drawLine(0, 51, 127, 51, SSD1306_WHITE);
  oled.setCursor(0, 54);
  if (WiFi.status() == WL_CONNECTED) oled.print(WiFi.localIP().toString());
  else oled.printf("Cut:%.2fV  Press=menu", c.cutoff);
  oled.display();
}

void draw_menu() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("CH%d Menu", sel_ch + 1);
  oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  for (int i = 0; i < N_ITEMS; i++) {
    int y = 12 + i * 12;
    if (i == sel_item) {
      oled.fillRect(0, y - 1, 128, 11, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }
    oled.setCursor(2, y);
    oled.print(ITEMS[i]);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

void draw_cutoff() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("CH%d Cutoff", sel_ch + 1);
  oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(20, 20);
  oled.printf("%.2fV", edit_val);
  oled.setTextSize(1);
  oled.setCursor(0, 46);
  oled.printf("%.2f - %.2f V", CUTOFF_MIN, CUTOFF_MAX);
  oled.setCursor(0, 55);
  oled.print("OK=press  Back=hold");
  oled.display();
}

void redraw() {
  switch (scr) {
    case SCR_CHAN:   draw_chan();   break;
    case SCR_MENU:   draw_menu();   break;
    case SCR_CUTOFF: draw_cutoff(); break;
  }
}

// ---------- Web server ----------
void srv_status() {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.createNestedArray("channels");
  for (int i = 0; i < NUM_CH; i++) {
    JsonObject o = arr.createNestedObject();
    o["ch"]     = i + 1;
    o["state"]  = st_str(ch[i].state);
    o["v"]      = String(ch[i].v,    3);
    o["i_ma"]   = String(ch[i].i_ma, 1);
    o["mah"]    = String(ch[i].mah,  2);
    o["esr"]    = String(ch[i].esr,  1);
    o["cutoff"] = String(ch[i].cutoff, 2);
  }
  String out; serializeJson(doc, out);
  srv.send(200, "application/json", out);
}

void srv_start() {
  if (!srv.hasArg("ch")) { srv.send(400, "text/plain", "missing ch"); return; }
  int c = srv.arg("ch").toInt() - 1;
  if (c < 0 || c >= NUM_CH) { srv.send(400, "text/plain", "bad ch"); return; }
  start_test(c);
  srv.send(200, "text/plain", "ok");
}

void srv_stop() {
  if (!srv.hasArg("ch")) { srv.send(400, "text/plain", "missing ch"); return; }
  int c = srv.arg("ch").toInt() - 1;
  if (c < 0 || c >= NUM_CH) { srv.send(400, "text/plain", "bad ch"); return; }
  stop_test(c);
  srv.send(200, "text/plain", "ok");
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_CH; i++) {
    pinMode(MOSFET_PINS[i], OUTPUT);
    gate_off(i);
    ce_on(i);
  }

  pinMode(ENC_A,  INPUT);
  pinMode(ENC_B,  INPUT);
  pinMode(ENC_SW, INPUT);
  attachInterrupt(digitalPinToInterrupt(ENC_A), enc_isr, RISING);

  Wire.begin(21, 22);
  tca(0);
  if (!ina.begin()) Serial.println("INA219 not found");
  ina.setCalibration_32V_2A();
  // 1-ohm resistors briefly exceed the 3.2A range at test start.
  // No setCalibration_32V_4A() in Adafruit library — readings clip briefly.

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("OLED not found at 0x3C");

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  oled.print("CellScan v0.1");
  oled.setCursor(0, 12); oled.print("WiFi...");
  oled.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);

  srv.on("/",           []{ srv.sendHeader("Location","/api/status"); srv.send(302); });
  srv.on("/api/status", srv_status);
  srv.on("/api/start",  HTTP_POST, srv_start);
  srv.on("/api/stop",   HTTP_POST, srv_stop);
  srv.begin();
}

uint32_t last_draw = 0;

void loop() {
  srv.handleClient();
  handle_ui();
  for (int i = 0; i < NUM_CH; i++) update_ch(i);
  if (millis() - last_draw >= 100) { redraw(); last_draw = millis(); }
}
