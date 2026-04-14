# CellScan — 4-Channel 18650 Battery Analyzer

## What It Does
CellScan tests up to 4 18650 lithium-ion cells simultaneously.
Each channel measures voltage, capacity (mAh), and internal resistance.
Results display on an OLED screen and a live WiFi dashboard hosted
by an ESP32, where you can start and stop tests per channel.

## Why I Built This
My science fair project was a 3S34P battery pack built from 34 salvaged
18650 cells. I bought a tester from Amazon but I wanted to understand how
it actually works and build one myself with 4 parallel channels so I could
test one series row at a time.

## System Architecture
[Cell 1] ──► [INA219 #1] ──┐
[Cell 2] ──► [INA219 #2] ──┤
[Cell 3] ──► [INA219 #3] ──┼──► [TCA9548A Mux] ──► [ESP32]
[Cell 4] ──► [INA219 #4] ──┘                          │
├──► [OLED Display]
└──► [WiFi Dashboard]
Each channel:
[Cell] ──► [INA219] ──► [MOSFET] ──► [2Ω 10W Resistor] ──► [GND]
▲
[ESP32 GPIO]

## How Each Measurement Works

**Voltage** — INA219 reads cell terminal voltage continuously every 500ms

**Capacity (mAh)** — ESP32 switches MOSFET on to start discharge through
load resistor. INA219 measures current. Capacity accumulates using coulomb
counting: `mAh += current_A * (elapsed_ms / 3600000.0)` every 500ms until
the cell hits the cutoff voltage.

**Internal Resistance** — Measured at test start using the voltage sag:
1. MOSFET off → read open circuit voltage (V_oc)
2. MOSFET on → wait 100ms → read loaded voltage (V_load) and current (I)
3. `R_int (mΩ) = (V_oc - V_load) / I * 1000`

## Wiring Diagram

[CellScan_Schematic.pdf](https://github.com/user-attachments/files/25781517/CellScan_Schematic.pdf)

## Bill of Materials

| Component | Qty | Unit Price | Link |
|-----------|-----|------------|------|
| ESP32 38-pin Dev Board | 1 | $8.99 | [Amazon](https://www.amazon.com/dp/B08D5ZD528) |
| INA219 Current Sensor (4-pack) | 1 | $10.99 | [Amazon](https://www.amazon.com/Bi-Directional-Breakout-Interface-Ar-duino-Raspberry/dp/B091DRHL79) |
| TCA9548A I2C Multiplexer | 1 | $5.99 | [Amazon](https://www.amazon.com/NOYITO-TCA9548A-Multiplexer-Breakout-Expansion/dp/B07DS6F3V2) |
| IRLZ44N MOSFET (5-pack) | 1 | $9.99 | [Amazon](https://www.amazon.com/Technologies-Threshold-Protective-Packaging-IRLZ44NPBF/dp/B0FMQCYG6Q/) |
| 2Ω 10W Power Resistor (10-pack) | 1 | $6.99 | [Amazon](https://www.amazon.com/Resistors-Wirewound-Resistance-Precharge-Horizontal/dp/B09V5MTX8Q/) |
| 0.96" OLED Display I2C SSD1306 | 1 | $6.99 | [Amazon](https://www.amazon.com/UCTRONICS-SSD1306-Self-Luminous-Display-Raspberry/dp/B072Q2X2LL/) |
| Single 18650 Cell Holders (4-pack) | 1 | $6.69 | [Amazon](https://www.amazon.com/Battery-Storage-Holder-Button-Single/dp/B07KD9JLH3/) |
| Breadboard + Jumper Wires | 1 | $15.99 | [Amazon](https://www.amazon.com/ELEGOO-Electronics-Potentiometer-tie-Points-Breadboard/dp/B09YRJQRFF/) |
| **Total** | | **~$72.62** | |

## Firmware
- `firmware/CellScan.ino` — full state machine per channel, coulomb counting, DCIR measurement, OLED display, WiFi dashboard

## Repository Structure
CellScan/
├── BOM.csv
├── README.md
├── firmware/
│   └── CellScan.ino
└── hardware/
└── CellScan_Schematic.kicad_sch
