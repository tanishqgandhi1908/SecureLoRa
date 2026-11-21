# Bill of materials

Prices are what we paid (DigiKey, Nov 14, 2026) or "kit" for course-kit parts. One node + one gateway.

| Part | Qty | Used in | Source | Price |
|---|---|---|---|---|
| ESP32 DevKit (ESP32-WROOM-32) | 2 | Node, gateway | Course kit + personal | kit |
| Adafruit RFM95W LoRa breakout 868/915 MHz (#3072) | 2 | Node, gateway | Kit (1), DigiKey 1528-1667-ND (1) | $19.95 |
| Adafruit 915 MHz spring antenna (#4269) | 2 | Both radios | Kit (1), DigiKey 1528-4269-ND (1) | $0.95 |
| DHT-11 temperature/humidity module | 1 | Node | Spare (SunFounder) kit | kit |
| ATECC608A breakout (SparkFun Qwiic) | 1 | Node (wired; not used, see ADR-2) | Course kit | kit |
| Adafruit INA219 current sensor (#904) | 1 | Gateway (optional current logging) | DigiKey 1528-904-ND | $9.95 |
| Red LED + 220 Ω resistor | 1 each | Node alert | Kit | kit |
| Breadboards, jumper wires | 2 + about 30 | Both | Kits | kit |
| USB cables + USB wall chargers | 2 | Power | Own | — |
| **Purchased total** | | | | **$30.85 + $13.99 shipping** |

The node board also carries the lab wiring (MPU-6050, OLED, servo, buzzer, buttons, potentiometer), held idle by the firmware. Server: any laptop that runs Docker.
