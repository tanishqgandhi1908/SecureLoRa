# SecureLoRa: Bench Test Results

Recorded **Fri Nov 20, 2026, 18:40–18:51** at the bench (node and gateway about 1 m apart, indoors). All values below are copied from the real Serial Monitor logs (gateway log: `gateway/logs/device-monitor-*.log`, timestamps from the PlatformIO `time` filter). Nothing here is estimated.

## Setup under test
| Part | Details |
|---|---|
| Node | ESP32 DevKit V1 (lab board), DHT-11 on GPIO 33, RFM95W 915 MHz, sends every **10 s** (bench setting; 60 s for the 72 h run) |
| Packet | `N=1;SEQ=<n>;T=<°C>;H=<%>;A=<0/1>;M=<tag>`, 62–65 bytes. Tag = first 16 bytes of HMAC-SHA256(pre-shared 256-bit key, everything before `;M=`) |
| Gateway | Second ESP32 + RFM95W; checks format → node id → HMAC → SEQ > last accepted (stored in flash), then HTTPS POST with bearer token |
| Radio | 915 MHz, SF7, 125 kHz, CR 4/5, 17 dBm, sync word 0xF3 |
| Server | FastAPI + InfluxDB + Grafana (Docker) on a teammate's laptop, HTTPS with our own CA |

## Results

| # | Test | How | Expected | Observed (log excerpt) | Result |
|---|---|---|---|---|---|
| 1 | Normal operation | Node sends every 10 s | Gateway `ACCEPTED`, server 200 | 18:41:15 `RX: N=1;SEQ=6;T=19.3;H=42.8;A=0;M=5dc1…` → `ACCEPTED (tag OK, SEQ fresh)` → `POST OK 200`. SEQ 6–10 all accepted and stored | ✅ Pass |
| 1b | HMAC is standard HMAC-SHA256 | Recomputed tags off-device with Python `hmac` + the same key | Tags of real packets match; tampered one doesn't | SEQ 6, 10 and 18 → **match**; tampered SEQ 10 (T=99.9) → **mismatch** | ✅ Pass |
| 2 | Replay attack | Node command `replay`: re-sends the last packet byte-for-byte | Rejected as replay | 18:41:58 `RX: N=1;SEQ=10;…;M=4b03…` → `REJECTED: replay (SEQ 10 <= last accepted 10)` | ✅ Pass |
| 3 | Modified packet | Node command `tamper`: last packet with T changed to 99.9, old tag | Rejected as bad signature | 18:42:05 `RX: N=1;SEQ=10;T=99.9;…` → `REJECTED: bad_signature (tag does not match: forged or modified)` | ✅ Pass |
| 4 | Forged packet | Node command `forge`: new SEQ 1011 with a made-up tag | Rejected as bad signature | 18:42:15 `RX: N=1;SEQ=1011;T=21.0;H=40.0;A=0;M=0011…eeff` → `REJECTED: bad_signature` | ✅ Pass |
| 5 | Node reboot | Node reset | SEQ continues from flash; gateway keeps accepting | Node: `Last SEQ from flash: 7` → next packet `SEQ=8` → gateway `ACCEPTED` | ✅ Pass |
| 6 | Gateway reboot | Gateway power cycle (18:45:45, `rst:0x1 POWERON_RESET`) | Last accepted SEQ survives | `Last accepted SEQ from flash: 33` → next packet SEQ 34 `ACCEPTED` | ✅ Pass |
| 6b | Replay after gateway reboot | Reboot gateway, then node `replay` | Still rejected | — | ⏳ Not run yet |
| 7 | Wi-Fi outage | Network dropped (hotspot change) | Readings still received, POST failures logged, auto-reconnect | 18:42:54 `Wi-Fi: LOST connection`; SEQ 16–17 `ACCEPTED` → `POST FAILED -100 (no Wi-Fi…)`; 18:43:10 `Wi-Fi: connected` (about 16 s) | ✅ Pass (readings during the outage are **dropped, not buffered**) |
| 8 | Server outage | `docker compose stop`, later `docker compose start` | Gateway keeps running, logs failures, recovers by itself | Every packet still `ACCEPTED` (SEQ 34 → 65 continuous, `missed=0`); POSTs `FAILED -1`; while the server restarted, `Connection reset by peer`; 18:50:27 `POST OK 200` again with no gateway action | ✅ Pass (outage readings **dropped**; each one logged with timestamp + SEQ) |
| 9 | Alert LED ("decide and act") | Threshold set just above room temp, sensor warmed by hand | `A=1` + red LED on, then off after cooling | Earlier bench test (with BMP280, threshold 30 °C): SEQ 3–5 `A=1` at 30.06–30.44 °C, LED on; SEQ 6 at 29.83 °C `A=0`, LED off | ✅ Pass (repeat with the DHT-11 for the demo) |

### Link quality at the bench (about 1 m)
RSSI **−65 to −71 dBm**, SNR **8.8 to 10.3 dB** on normal packets. One packet at 18:49:55 arrived at **−90 dBm / SNR 2.5** (board moved or blocked) and was still received and accepted.

### HTTPS POST timing
Gateway receive → `POST OK 200` took about **0.9–1.5 s** typically (TLS handshake per request), and once **4.5 s** (18:41:44 → 18:41:48).

## Problems found during testing
| Problem | Cause | Fix | Status |
|---|---|---|---|
| 2 real packets lost (`NOTE: 2 packet(s) missed (SEQ 10 -> 13)`) | The node sent real SEQ 11 / 12 right after the `tamper` / `forge` test packets. The gateway's radio was idle while it POSTed the rejection (about 1 s), so the real packet was missed. An attacker could use this to cause loss by flooding junk packets | The gateway now puts the radio back into receive mode **before** handling/POSTing a packet (`LoRa.receive()`) | Fix built; **re-test pending** (re-upload the gateway, then `tamper` → the next real SEQ must be accepted with no `missed`) |
| All POSTs failed after the network change | Laptops moved to a phone hotspot, so the server IP changed (the server cert is tied to the IP) | Use one stable network; if the IP changes, re-run `generate_certs.sh <new IP>` and update `SERVER_URL` | Resolved |
| ESP32 HTTPS `-9984` (earlier) | mbedTLS 2.28 on the ESP32 only matches DNS-name SANs, not IP SANs | The cert script lists the IP as both DNS and IP SAN | Resolved |
| Node: `I2C BUS STUCK: SDA/SCL LOW` | An I2C module (ATECC608A/MPU-6050) probably lost power and holds the lines low | None needed: nothing in the project uses I2C on the node now (HMAC replaced the ATECC608A) | Known, harmless |

## Not measured yet
- **Node current (INA219):** logging works (`INA219 OK`), but VIN+/VIN− are not in the node's supply yet, so it reads −0.10 mA / 0.88 V (floating). Prediction to compare against: 6.8 mA (assumed deep sleep, 60 s cycle).
- **Range at a realistic distance:** only bench distance so far.
- **72 h unattended run.**
