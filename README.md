# SecureLoRa (CS244P Group 19)

A BMP280 node in a shed sends temperature over 915 MHz LoRa to a gateway in the house, which forwards it over Wi-Fi to a server and dashboard.
**Current stage: vertical slice.** Packets are plain text and **not signed yet**.

```
SecureLoRa/
  shared/    iot_pins.h, iot_common.h (copied from the course labs), lora_settings.h (radio settings for both boards)
  node/      PlatformIO project: BMP280 -> LoRa, LED alert
  gateway/   PlatformIO project: LoRa -> Wi-Fi -> HTTP(S) POST
```
Open `node/` or `gateway/` as its own project in VS Code (PlatformIO: File → Open Folder).

## Wiring (course kit pin map, same as the labs)

**Both boards, RFM95W LoRa radio:**
| RFM95W | ESP32 |
|---|---|
| VIN | 3V3 |
| GND | GND |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| CS | GPIO 14 |
| RST | GPIO 26 |
| G0 (DIO0) | GPIO 25 |
| ANT | 915 MHz spring antenna (solder it on **before** powering) |

**Node only:**
| Part | ESP32 |
|---|---|
| BMP280 + ATECC608A: VCC / GND | 3V3 / GND |
| BMP280 + ATECC608A: SDA / SCL | GPIO 21 / GPIO 22 |
| BMP280 SDO / CSB | GND (address 0x76) / 3V3 |
| Alert LED | GPIO 2 → 220 Ω → LED → GND (GPIO 2 is also the onboard LED) |

The SPI OLED from the labs can stay wired (CS 5). The code keeps it switched off.

## Run it

**Node:** open `node/`, then Upload → Serial Monitor. You should see the I2C scan, `BMP280 OK`, `LoRa OK`, and a packet every 10 s:
```
Packet: N=1;SEQ=3;T=23.41;P=1012.6;A=0  (31 bytes)  SENT
```

**Gateway:**
1. Copy `gateway/include/secrets_example.h` to `gateway/include/secrets.h` and fill in Wi-Fi + server URL. This file is git-ignored.
2. Upload → Serial Monitor. Every packet from the node is printed with RSSI/SNR and POSTed as JSON:
```json
{"node_id":1,"seq":3,"type":"reading","temp_c":23.41,"pressure_hpa":1012.6,"alert":false,"rssi_dbm":-45,"snr_db":9.8,"verdict":"unverified"}
```
3. No second radio yet? Type `fake` + Enter in the gateway's Serial Monitor. It POSTs one reading labelled `"type":"test"`, so you can test the server path.

## Not done yet (in build order)
1. ATECC608A signature on the node + verification on the gateway (`verdict` becomes accepted/rejected)
2. Session + sequence replay checks, join handshake
3. Flash ring buffer on the gateway for Wi-Fi/server outages
4. Deep sleep on the node, 60 s interval, current measurement with the INA219
5. HTTPS only (no plain http) + server token

## Security notes
- No passwords/tokens in Git: use `secrets.h` (ignored) and `secrets_example.h` (committed).
- The LoRa sync word is **not** security. It only filters out other teams' radios.
- The node accepts no over-the-air commands. Firmware is USB-only.
