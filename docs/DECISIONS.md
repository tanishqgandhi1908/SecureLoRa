# Design decisions (short ADRs)

Each entry: the decision, why, and what we gave up. Dates are when the decision was made.

## ADR-1: LoRa 915 MHz, SF7 / 125 kHz (Oct 2026)
- **Decision:** a point-to-point LoRa link (no LoRaWAN), SF7, 125 kHz, CR 4/5, 17 dBm, sync word 0xF3.
- **Why:** the shed has no dependable Wi-Fi; a reading is under 70 bytes; SF7 keeps airtime around 110 ms. The sync word only filters out other teams' radios.
- **Gave up:** LoRaWAN's network server, adaptive data rate and built-in security. Measured: 20–30 m through 2 walls at −75 to −92 dBm.

## ADR-2: HMAC-SHA256 with a pre-shared key instead of ATECC608A ECDSA (Nov 20)
- **Decision:** the node appends the first 16 bytes of HMAC-SHA256(key, body) as `;M=<hex>`. The 256-bit key lives in git-ignored `shared/secret_key.h`, the same file for both boards.
- **Why:** this is the "authenticated-message" fallback named in the proposal. It uses mbedTLS already in the ESP32 core, needs no irreversible chip locking, and took hours, not days.
- **Gave up:** the key sits in normal flash, so a stolen board can be read and used to forge packets; one shared key for all nodes. The ATECC608A would keep the private key inside the chip.

## ADR-3: Persisted SEQ instead of a join handshake (Nov 20)
- **Decision:** the node writes SEQ to flash (NVS) **before** each send; the gateway accepts only SEQ greater than the last accepted one, also kept in flash.
- **Why:** the progress report's stated simplification. Replays are rejected across node and gateway reboots without two-way radio traffic.
- **Gave up:** if the node's flash is fully erased, its SEQ restarts at 0 and the gateway must be told (`resetseq` over USB).

## ADR-4: Check order format → node → HMAC → SEQ (Nov 20)
- **Decision:** the tag is checked before SEQ.
- **Why:** an unauthenticated packet's SEQ means nothing, so a tampered packet is reported as `bad_signature`, not `replay`. HMAC is cheap enough to run on every packet.

## ADR-5: Forward rejections, rate-limited; keep them out of link health (Nov 20)
- **Decision:** rejected packets are sent to the server as `type:"rejected"`, at most one per 5 s; the server counts only `type:"reading"` with an accepted/unverified verdict for SEQ gaps, delivery rate and staleness.
- **Why:** the dashboard should show attacks, but an attacker-chosen SEQ must not distort the statistics, and a flood must not flood the server.

## ADR-6: DHT-11 instead of BMP280 (Nov 19–20)
- **Decision:** temperature ±2 °C + humidity on GPIO 33; pressure became optional everywhere.
- **Why:** the BMP280 failed and the DHT-11 was on hand. GPIO 33 is shared with lab Button 2 because every other pin is used by lab wiring.
- **Gave up:** pressure and some accuracy.

## ADR-7: USB / 5 V power, no deep sleep (Nov 14)
- **Decision:** no battery or charger; node always awake.
- **Why:** cost, and the DevKit's regulator can brown out on a single Li-ion cell. Current measurement was optional (confirmed with the professor).
- **Gave up:** the battery-life claim; the 6.8 mA budget stays a prediction.

## ADR-8: No outage buffer on the gateway (Nov 20)
- **Decision:** if Wi-Fi or the server is down, the accepted reading is dropped and the failure is logged with time and SEQ.
- **Why:** keeps the gateway simple; every gap stays explainable from the logs.
- **Gave up:** data during outages. A flash ring buffer is the first production fix.

## ADR-9: HTTPS with our own CA, server addressed by IP (Nov 16–20)
- **Decision:** the gateway pins our CA (`setCACert`, never `setInsecure`) and sends a write-only bearer token. The server certificate lists the IP as both a DNS name and an IP address.
- **Why:** the ESP32's mbedTLS 2.28 only matches DNS-name entries, so an IP-only certificate fails with `-9984`.
- **Gave up:** a changing server IP breaks HTTPS until the certificate is regenerated (see `RUNBOOK.md`).

## ADR-10: Re-arm the radio before the HTTPS POST (Nov 20)
- **Decision:** the gateway calls `LoRa.receive()` right after reading a packet, before handling it.
- **Why:** the POST blocks 1–5 s; testing showed real packets arriving during it were lost, which an attacker could trigger with junk packets.
