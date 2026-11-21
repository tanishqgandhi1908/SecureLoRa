# Runbook: what to do when something breaks

Look at the **gateway Serial Monitor** first (timestamped, also saved in `gateway/logs/`). Most problems show up there as one clear line.

## Dashboard says data is stale (no reading for 3+ minutes)
| Gateway shows | Meaning | Do this |
|---|---|---|
| No `RX:` lines at all | Gateway hears nothing | Check the node has power and its Monitor shows `SENT`; check both antennas; move the boards closer |
| `RX:` + `POST FAILED -100` | Gateway Wi-Fi is down | Check the network; the gateway retries every 10 s by itself |
| `RX:` + `POST FAILED -1` | Server unreachable, down, or certificate rejected | Server running (`docker compose ps`)? Server IP changed? (next section) |
| `RX:` + `POST FAILED 401` | Token wrong | `API_TOKEN` in `gateway/include/secrets.h` must equal `GATEWAY_API_TOKEN` in `server/.env` |
| `RX:` + `POST FAILED 503` | Server up, InfluxDB write failed | `docker compose logs influxdb`; restart it |
| `RX:` + `REJECTED: bad_signature` on every packet | Node and gateway were built with different keys | Rebuild both from the same `shared/secret_key.h` |
| `RX:` + `REJECTED: replay` on every packet | Node's SEQ restarted (flash erased) | Type `resetseq` in the gateway Monitor |

## Server IP changed (e.g. new network or hotspot)
1. On the server laptop: `ipconfig getifaddr en0` → new IP.
2. `cd server && ./scripts/generate_certs.sh <new IP>` (keeps the same CA).
3. `docker compose restart reverse_proxy`.
4. In `gateway/include/secrets.h`: `SERVER_URL "https://<new IP>:8443/api/readings"`, then re-upload the gateway. `SERVER_ROOT_CA` does not change.

## Node problems
| Node shows | Do this |
|---|---|
| `DHT-11 NOT RESPONDING` / `read FAILED` | Check `+` → 3V3, `−` → GND, `S` → GPIO 33; don't press lab Button 2 |
| `LoRa INIT FAILED` + `version register = 0x0` | Radio has no power: VIN → 3V3, GND |
| `LoRa INIT FAILED` + `version register = 0xFF` | CS 14 / SCK 18 / MISO 19 unplugged or in the wrong row |
| `HMAC KEY MISSING/INVALID` | Create `shared/secret_key.h` from the example (`openssl rand -hex 32`) |
| `I2C BUS STUCK` | Harmless for the project (nothing uses I2C on the node now) |

## Before a long run or the demo
- Node `SEND_INTERVAL_MS` = 60000; threshold `TEMP_THRESHOLD_C` = 35.0.
- Fresh database: `docker compose down -v && docker compose up -d`, then regenerate the cert and restart the proxy (above).
- One stable network for all devices; server laptop never sleeps.
- Don't type `fake` and don't press Button 2 during a run.
- Uploading with two ESP32s plugged in: plug in one at a time.

## Rotating secrets
- **Server token:** `openssl rand -hex 32` → `GATEWAY_API_TOKEN` in `server/.env` → `docker compose up -d` → same value in the gateway's `secrets.h` → re-upload.
- **HMAC key:** new `shared/secret_key.h` → re-upload **both** boards → `resetseq` is not needed (SEQ continues).
