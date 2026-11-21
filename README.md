# SecureLoRa (CS244P Group 19)

A DHT-11 node in a shed sends temperature and humidity over 915 MHz LoRa to a gateway in the house, which forwards it over Wi-Fi to a server and dashboard.
Every packet carries an **HMAC-SHA256 tag** (pre-shared key) and a **SEQ counter saved in flash**. The gateway rejects forged/modified packets (`bad_signature`) and replays (`replay`).

```
SecureLoRa/
  shared/    iot_pins.h, iot_common.h (copied from the course labs), lora_settings.h (radio settings for both boards),
             packet_auth.h (HMAC tag), lora_diag.h (LoRa wiring check), secret_key_example.h (template for the key)
  node/      PlatformIO project: DHT-11 -> LoRa, LED alert
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
| DHT-11: `+` / `-` / `S` | 3V3 / GND / **GPIO 33** (shared with lab Button 2: don't press it while the project runs) |
| ATECC608A: VCC / GND / SDA / SCL | 3V3 / GND / GPIO 21 / GPIO 22 |
| Alert LED | GPIO 2 → 220 Ω → LED → GND (GPIO 2 is also the onboard LED) |

The SPI OLED from the labs can stay wired (CS 5). The code keeps it switched off.

**Sensor change (Nov 20):** the original BMP280 (temperature + pressure) failed, so the node now uses the DHT-11 (temperature ±2 °C + humidity) from a spare kit. Packets carry humidity (`H=`) instead of pressure (`P=`). The gateway and server still accept pressure if a BMP280 comes back.

## Run it

**First, the shared key (both boards):** copy `shared/secret_key_example.h` to `shared/secret_key.h` and put a random key in it:
```bash
openssl rand -hex 32
```
Both the node and the gateway must be built with the **same** `secret_key.h`. It is git-ignored: share it privately.

**Node:** open `node/`, then Upload → Serial Monitor. You should see the I2C scan, `DHT-11 OK`, `LoRa OK`, and a packet every 10 s:
```
Packet: N=1;SEQ=3;T=23.0;H=41.0;A=0;M=<32 hex chars>  (63 bytes)  SENT
```
SEQ continues from the last value saved in flash, so it never repeats, even after a reboot.

**Gateway:**
1. Copy `gateway/include/secrets_example.h` to `gateway/include/secrets.h` and fill in Wi-Fi + server URL. This file is git-ignored.
2. Upload → Serial Monitor. Every packet from the node is printed with RSSI/SNR and POSTed as JSON:
```json
{"node_id":1,"seq":3,"type":"reading","temp_c":23.00,"humidity_pct":41.0,"alert":false,"rssi_dbm":-45,"snr_db":9.8,"verdict":"accepted"}
```
3. Gateway Serial Monitor commands: `fake` POSTs one `"type":"test"` record (don't use it during real runs). `resetseq` forgets the stored last-accepted SEQ (only needed if the node's flash was fully erased).

## Gateway checks (in this order)
| Check | Fails as | Forwarded to server? |
|---|---|---|
| Has N, SEQ, T and a `;M=` tag | `malformed` | no (counted + printed) |
| Node id is one we have a key for (1) | `unknown_node` | yes, `type:"rejected"` (max 1 per 5 s) |
| HMAC tag matches the body | `bad_signature` | yes, `type:"rejected"` (max 1 per 5 s) |
| SEQ > last accepted SEQ (stored in flash) | `replay` | yes, `type:"rejected"` (max 1 per 5 s) |
| all OK | `accepted` | yes, `type:"reading"` |

## Fault tests (node Serial Monitor over USB)
| Command | What the node sends | Gateway should print |
|---|---|---|
| `replay` | the last packet again, unchanged | `REJECTED: replay` |
| `tamper` | the last packet with T changed to 99.9, old tag | `REJECTED: bad_signature` |
| `forge` | a new packet with a made-up tag | `REJECTED: bad_signature` |
Set `ENABLE_ATTACK_TESTS 0` in `node/src/main.cpp` to remove these commands. They only work over USB, and they can only send packets the gateway must reject.

## Current measurement
The gateway reads an INA219 (I2C 21/22) placed in the node's 5 V supply (VIN+ from the supply, VIN- to the node's VIN pin; node USB unplugged). It prints `POWER: avg/min/max mA ...` once per minute. The gateway Serial Monitor saves timestamped logs to `gateway/logs/`.

## Not done (kept simple on purpose)
- ATECC608A signing (HMAC with a pre-shared key is used instead: the proposal's fallback design)
- Join handshake / session IDs (a SEQ persisted in flash on both sides is used instead)
- Buffering readings during Wi-Fi/server outages (they are dropped and logged)
- Deep sleep on the node; the 60 s interval (`SEND_INTERVAL_MS`) is set for the 72 h run

## Security notes
- No passwords/tokens/keys in Git: `secrets.h` and `shared/secret_key.h` are ignored; the `_example.h` files are committed.
- HMAC gives integrity and authenticity, **not secrecy**: anyone can read the readings over the air.
- The key sits in normal flash on both boards: someone who steals a board can read it and forge packets (residual risk; the ATECC608A would fix this).
- The LoRa sync word is **not** security. It only filters out other teams' radios.
- The node accepts no over-the-air commands. Firmware is USB-only.
