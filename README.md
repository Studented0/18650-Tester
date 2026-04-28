# CellScan
 
A 4-channel 18650 battery analyzer. Tests up to 4 cells at once and measures voltage, capacity in mAh, and internal resistance per channel. Results show on a small OLED with a rotary encoder to navigate, and a WiFi dashboard to start and stop tests remotely/edit settings.
 
---
 
## Screenshots
 
<img width="2983" height="4096" alt="Schematic" src="https://github.com/user-attachments/assets/468e721d-cd10-43aa-bbcf-cbd18dc805ad" />
<img width="1728" height="967" alt="PCB front" src="https://github.com/user-attachments/assets/225b0c0e-0082-48da-9bba-389f472be4ed" />
<img width="1724" height="978" alt="PCB back" src="https://github.com/user-attachments/assets/25741943-ebc5-4494-92fd-266fa26940e5" />
---
 
## Why I built this
 
My science fair project was a 3S34P battery pack from 34 salvaged 18650 cells. I bought a cheap tester off Amazon to sort them but I wanted to know how it actually worked. I also wanted an excuse to learn PCB design. CellScan is the result, a two-layer PCB with SMD assembly through JLCPCB.
 
---
 
## Hardware
 
Two-layer board, 100×150mm, designed in KiCad 9. SMD components assembled by JLCPCB. Through-hole parts soldered by hand after the board arrives: four MOSFETs, four cement resistors, four cell holders, the OLED header, the ESP32 dev board, and the rotary encoder. The cement resistors run hot under load so they sit raised slightly off the board.
 
Each channel has an INA219 current sensor, an IRLZ44N MOSFET as a discharge switch, and a 1Ω 10W cement resistor as the load. All four INA219s share an I2C bus through a TCA9548A multiplexer since they would otherwise have the same address. Each channel also has a TP4056 lithium charger for charging and discharging. 
 
---
 
## How it measures
 
**Voltage** — INA219 reads bus voltage continuously on each channel.
 
**Capacity** — ESP32 switches the MOSFET on to start discharge. Capacity accumulates by coulomb counting every 500ms until the cell hits the cutoff voltage.
 
`mAh += current_mA * (elapsed_ms / 3600000.0)`
 
**Internal resistance (DCIR)** — Measured at test start using voltage sag. MOSFET off, read open-circuit voltage. MOSFET on, wait 100ms, read loaded voltage and current.
 
`ESR (mΩ) = (V_oc - V_load) / I_load * 1000`
 
---
 
## Rotary Encoder
 
EC11 encoder wired directly to the ESP32. Rotate to switch between channels, press to open a menu, long press to go back. From the menu you can start a test, stop it, or adjust the cutoff voltage per channel. 
 
### Wiring
 
| Signal | GPIO |
|--------|------|
| ENC_A | 15 |
| ENC_B | 2 |
| ENC_SW | 5 |
| SDA | 21 |
| SCL | 22 |
 
---
 
## Firmware
 
`firmware/CellScan.ino` — state machine per channel, coulomb counting, DCIR measurement, TP4056 CE control, rotary encoder UI, OLED display, WiFi dashboard.
 
Libraries needed: Adafruit INA219, Adafruit SSD1306, Adafruit GFX, ArduinoJson. Install through Arduino IDE library manager. Board: ESP32 Dev Module.
 
---
 
## Bill of Materials
 
SMD parts ordered through JLCPCB assembly. Through-hole parts sourced separately.
 
| Component | Qty | LCSC | Note |
|-----------|-----|------|------|
| 100nF 0402 cap | 11 | C1525 | Basic |
| 10kΩ 0402 resistor | 7 | C25744 | Basic |
| 5.1kΩ 0402 resistor | 2 | C25905 | Basic — USB-C CC pull-downs |
| 1.2kΩ 0402 resistor | 4 | C25862 | Extended — TP4056 PROG |
| 1kΩ 0402 resistor | 8 | C11702 | Basic — LED series |
| Red LED 0402 | 8 | C71911 | Extended |
| INA219AIDR | 4 | C138706 | Extended |
| TCA9548AMRGER | 1 | C2876717 | Extended |
| TP4056 SOP-8 | 4 | C16581 | Extended |
| GCT USB4125 USB-C | 1 | C2682777 | Extended |
 
| Component | Qty | Link |
|-----------|-----|------|
| ESP32 WROOM-32 dev board | 1 | [Amazon](https://www.amazon.com/dp/B08D5ZD528) |
| IRLZ44N MOSFET | 4 | [Amazon](https://www.amazon.com/Technologies-Threshold-Protective-Packaging-IRLZ44NPBF/dp/B0FMQCYG6Q/) |
| 1Ω 10W cement resistor | 4 | [Amazon](https://www.amazon.com/Resistors-Wirewound-Resistance-Precharge-Horizontal/dp/B09V5MTX8Q/) |
| 18650 cell holders | 4 | [Amazon](https://www.amazon.com/Battery-Storage-Holder-Button-Single/dp/B07KD9JLH3/) |
| SSD1306 OLED 0.96" | 1 | [Amazon](https://www.amazon.com/UCTRONICS-SSD1306-Self-Luminous-Display-Raspberry/dp/B072Q2X2LL/) |
| EC11 rotary encoder | 1 | already owned |
 
---
 
## Repository Structure
 
```
18650-Tester/
├── BOM.csv
├── README.md
├── firmware/
│   └── CellScan.ino
└── hardware/
    ├── CellScan_Schematic.kicad_sch
    ├── CellScan_Schematic.kicad_pcb
    ├── CellScan_Schematic.kicad_pro
    ├── CellScan_Schematic.pdf
    ├── libraries/
    │   └── ESP32-DevKitC-30PIN.kicad_mod
    └── gerbers/
```
 
---
 
## License
 
[MIT](LICENSE)
 
