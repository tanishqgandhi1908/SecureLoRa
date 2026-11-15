# SecureLoRa Server & Dashboard (CS244P Group 19)

Ingest service, InfluxDB 2.x storage, and Grafana monitoring dashboard for SecureLoRa.

Receives sensor telemetry from the ESP32 gateway over Wi-Fi via HTTP/HTTPS POST, validates the payload contract, persists time-series readings, tracks RF link health and sequence continuity, and visualizes live and historical environmental data.

---

## 1. Architecture Overview

```
 [ Node (Shed) ]                     [ Gateway (House) ]                 [ Server / Laptop ]
 BMP280 + ATECC608A                   ESP32 + RFM95W                      Docker Compose
         |                                   |                                   |
         |---- 915 MHz LoRa (81B binary) --->|                                   |
                                             |---- Wi-Fi HTTP(S) POST ---------->| Nginx Reverse Proxy (8080/8443)
                                                   Authorization: Bearer <token> |      |
                                                   JSON reading payload          |      v
                                                                                 | FastAPI Ingest (:8000)
                                                                                 |      |
                                                                                 |      +---> Audit Log (logs/ingest.jsonl)
                                                                                 |      |
                                                                                 |      v
                                                                                 | InfluxDB 2.7 (:8086)
                                                                                 |      ^
                                                                                 |      |
                                                                                 | Grafana Dashboard (:3000)
```

### Components
- **Ingest Service (FastAPI / Uvicorn)**:
  - Validates JSON payload types and required fields strictly.
  - Enforces write-only `Authorization: Bearer <API_TOKEN>` authentication.
  - Adds server reception timestamps (`server_rx_time`).
  - Tracks sequence continuity (`seq`), lost packets, delivery rates, and inter-packet gaps.
  - Automatically flags link staleness if no packet is received for > 3 minutes (180s).
  - Maintains a structured audit log (`logs/ingest.jsonl`) for 72-hour reliability analysis.
- **Reverse Proxy (Nginx)**:
  - Dual-port architecture: exposes plain HTTP on `:8080` (for rapid bench tests) and HTTPS on `:8443` (for production TLS verification).
- **InfluxDB 2.7**:
  - Measurements: `reading`, `packet_event`, `link_health`.
- **Grafana 10.4**:
  - Pre-provisioned datasource (InfluxDB Flux) and dashboard (`securelora_dashboard.json`).

---

## 2. Quick Start

### Prerequisites
- [Docker](https://docs.docker.com/get-docker/) & [Docker Compose](https://docs.docker.com/compose/)
- Python 3.10+ (for local test scripts)
- OpenSSL (pre-installed on macOS/Linux)

### Step 1: Clone & Configure
```bash
cd server

# 1. Copy template environment file
cp .env.example .env

# 2. (Optional) Customize tokens or passwords in .env
# Default gateway token: securelora_gateway_write_token_2026_demo
```

### Step 2: Generate TLS Certificates
Generate self-signed TLS certificates for secure HTTPS communication:
```bash
chmod +x scripts/generate_certs.sh
./scripts/generate_certs.sh
```
This generates `certs/ca.pem`, `certs/server.crt`, and `certs/server.key`.

### Step 3: Launch Containers
```bash
docker compose up -d
```

Verify all 4 containers are running and healthy:
```bash
docker compose ps
```
You should see:
- `securelora_influxdb` (healthy on port 8086)
- `securelora_ingest` (healthy on internal port 8000)
- `securelora_proxy` (listening on 8080 HTTP and 8443 HTTPS)
- `securelora_grafana` (listening on 3000)

---

## 3. Web Interfaces & Credentials

| Service | URL | Default Username | Default Password | Notes |
|---|---|---|---|---|
| **Grafana Dashboard** | http://localhost:3000 | `dashboard_admin` | `SecureLoRaDashboard2026!Password` | Anonymous viewing enabled |
| **Ingest Health** | http://localhost:8080/health | N/A | N/A | Returns JSON status |
| **InfluxDB UI** | http://localhost:8086 | `securelora_admin` | `SecureLoRaAdmin2026!Password` | Org: `securelora`, Bucket: `readings` |

---

## 4. Testing Without Hardware

### Automated Test Suite
Run the automated test suite to verify all endpoints, error handling, and authentication:
```bash
python3 scripts/test_ingest.py
```
Expected output:
- `[PASS] Service Health Check`
- `[PASS] POST valid reading (200 OK)`
- `[PASS] POST test reading (type='test') (200 OK)`
- `[PASS] Missing Authorization header (401 Unauthorized)`
- `[PASS] Invalid Bearer token (401 Unauthorized)`
- `[PASS] Malformed JSON syntax (400 Bad Request)`
- `[PASS] Missing required field (400 Bad Request)`
- `[PASS] Invalid field types (400 Bad Request)`
- `[PASS] High temp alert packet (200 OK)`
- `[PASS] Sequence gap detection (200 OK)`
- `[PASS] HTTPS Ingest with CA certificate (200 OK)`

### Manual Curl Tests

**1. Send a valid reading:**
```bash
curl -i -X POST http://localhost:8080/api/readings \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer securelora_gateway_write_token_2026_demo" \
  -d '{"node_id":1,"seq":1,"type":"reading","temp_c":24.50,"pressure_hpa":1013.2,"alert":false,"rssi_dbm":-50,"snr_db":9.5,"verdict":"unverified"}'
```
Response:
```json
{"status":"ok","node_id":1,"seq":1,"verdict":"unverified","server_rx_time":1794683400.123}
```

**2. Send a fake test reading (gateway `fake` command equivalent):**
```bash
curl -i -X POST http://localhost:8080/api/readings \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer securelora_gateway_write_token_2026_demo" \
  -d '{"node_id":0,"seq":0,"type":"test","temp_c":0.0,"pressure_hpa":0.0,"alert":false,"verdict":"test"}'
```

**3. Test wrong token (returns 401):**
```bash
curl -i -X POST http://localhost:8080/api/readings \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer invalid_token" \
  -d '{"node_id":1,"seq":1,"type":"reading","temp_c":24.50,"pressure_hpa":1013.2,"alert":false,"rssi_dbm":-50,"snr_db":9.5,"verdict":"unverified"}'
```

**4. Test malformed JSON (returns 400):**
```bash
curl -i -X POST http://localhost:8080/api/readings \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer securelora_gateway_write_token_2026_demo" \
  -d '{"node_id":1, "temp_c": "invalid_temp"}'
```

---

## 5. Gateway Simulator (Live Dashboard Testing)

Run the simulator script to stream continuous readings into InfluxDB and watch Grafana update live:
```bash
# Stream readings every 5 seconds
python3 scripts/simulate_gateway.py

# Simulate high temperature alert (>35 °C)
python3 scripts/simulate_gateway.py --alert

# Simulate a packet drop / sequence gap
python3 scripts/simulate_gateway.py --simulate-gap

# Send over HTTPS
python3 scripts/simulate_gateway.py --https
```

---

## 6. What to Send Tanishq (For `gateway/include/secrets.h`)

Send these values privately (e.g. over Slack / email, NEVER in Git):

1. **Server URL:**
   - **Bench test (HTTP):** `http://<YOUR_LAPTOP_IP>:8080/api/readings`
   - **Production (HTTPS):** `https://<YOUR_LAPTOP_IP>:8443/api/readings`
   *(Find your laptop IP using `ipconfig getifaddr en0` on macOS or `hostname -I` on Linux).*
2. **API Token:**
   `securelora_gateway_write_token_2026_demo` (or custom token from `.env`)
3. **CA Certificate (for HTTPS):**
   Copy the contents of `server/certs/ca.pem` into `SERVER_ROOT_CA`:
   ```cpp
   static const char SERVER_ROOT_CA[] = R"PEM(
   -----BEGIN CERTIFICATE-----
   ... (contents of ca.pem) ...
   -----END CERTIFICATE-----
   )PEM";
   ```
4. **Grafana Dashboard Link:**
   `http://<YOUR_LAPTOP_IP>:3000/d/securelora-main/`

---

## 7. 72-Hour Reliability Test Execution & Analysis

During the 72-hour continuous test (must start by Nov 20):
1. Keep the server running (`docker compose up -d`). Containers have `restart: unless-stopped`.
2. All received packets, sequence numbers, and any HTTP errors are logged in real-time to `server/logs/ingest.jsonl`.
3. To monitor logs live:
   ```bash
   docker compose logs -f ingest
   ```
4. After the 72-hour test completes, run the analyzer:
   ```bash
   python3 scripts/analyze_logs.py --nominal-interval 60.0
   ```
   This generates:
   - Summary statistics in terminal: uptime, total requests, packets received vs expected, delivery rate (%).
   - Itemized list of every sequence gap and outage duration > 65s.
   - `reliability_report_72h.md` formatted for inclusion directly into the project report.

---

## 8. InfluxDB Schema Reference

### Measurement: `reading`
- **Tags**:
  - `node_id`: ID of transmitting node (`"1"`, etc.)
  - `session_id`: Session hex identifier (`"none"` or hex string)
  - `type`: Message type (`"reading"`, `"test"`, `"heartbeat"`, `"alert"`)
  - `verdict`: Packet status (`"unverified"`, `"accepted"`, `"bad_signature"`, `"replay"`, `"test"`)
  - `is_test`: `"true"` for test messages, `"false"` for real readings
- **Fields**:
  - `temp_c` (float): Temperature in °C
  - `pressure_hpa` (float): Barometric pressure in hPa
  - `alert` (bool): True if temperature exceeded threshold
  - `alert_int` (int): 1 if alert, 0 otherwise
  - `seq` (int): Monotonically increasing sequence number
  - `rssi_dbm` (float): Received signal strength in dBm
  - `snr_db` (float): Signal-to-noise ratio in dB
  - `server_rx_time` (float): Server receipt epoch timestamp

### Measurement: `packet_event`
- **Tags**: `verdict`, `node_id`, `type`
- **Fields**: `seq` (int), `rssi` (float), `snr` (float), `reason` (string)

### Measurement: `link_health`
- **Tags**: `node_id`
- **Fields**: `delivery_rate_pct` (float), `gap_seconds` (float), `stale` (bool), `stale_int` (int)
