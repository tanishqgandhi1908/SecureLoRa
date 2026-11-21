#!/usr/bin/env python3
"""
simulate_gateway.py - Gateway Simulator for SecureLoRa

Simulates the ESP32 gateway forwarding BMP280 readings over HTTP or HTTPS.
Use this to test InfluxDB ingest, Grafana dashboard rendering, alert triggers,
packet loss / sequence gaps, and stale data banners without physical hardware.
"""

import argparse
import random
import os
import sys
import time
from pathlib import Path
import requests

script_dir = Path(__file__).resolve().parent
server_dir = script_dir.parent
env_file = server_dir / ".env"

token = os.environ.get("GATEWAY_API_TOKEN", "")
http_port = 8080
https_port = 8443

if env_file.exists():
    with open(env_file, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("GATEWAY_API_TOKEN="):
                token = line.split("=", 1)[1].strip()
            elif line.startswith("HTTP_PORT="):
                http_port = int(line.split("=", 1)[1].strip())
            elif line.startswith("HTTPS_PORT="):
                https_port = int(line.split("=", 1)[1].strip())

if not token or token == "CHANGE_ME":
    sys.exit("GATEWAY_API_TOKEN is not set. Put it in server/.env (see .env.example).")

CA_CERT = server_dir / "certs" / "ca.pem"


def parse_args():
    parser = argparse.ArgumentParser(description="Simulate SecureLoRa ESP32 Gateway")
    parser.add_argument("--url", default=f"http://localhost:{http_port}/api/readings", help="Server URL")
    parser.add_argument("--token", default=token, help="Bearer API token")
    parser.add_argument("--node", type=int, default=1, help="Node ID (default: 1)")
    parser.add_argument("--interval", type=float, default=5.0, help="Interval between readings in seconds (default: 5.0)")
    parser.add_argument("--count", type=int, default=0, help="Number of packets to send (0 = infinite)")
    parser.add_argument("--alert", action="store_true", help="Force high temperature alert (>35 C)")
    parser.add_argument("--simulate-gap", action="store_true", help="Simulate a packet drop (skip 3 sequence numbers)")
    parser.add_argument("--fake-test", action="store_true", help="Send a gateway 'fake' test packet")
    parser.add_argument("--https", action="store_true", help="Send to HTTPS endpoint (port 8443)")
    return parser.parse_args()


def main():
    args = parse_args()
    url = args.url
    if args.https and not url.startswith("https://"):
        url = f"https://localhost:{https_port}/api/readings"

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {args.token}",
    }

    verify_ca = str(CA_CERT) if CA_CERT.exists() and url.startswith("https://") else False

    print("=" * 65)
    print(f"SecureLoRa Gateway Simulator")
    print(f"Target URL:     {url}")
    print(f"Node ID:        {args.node}")
    print(f"Interval:       {args.interval}s")
    print(f"Alert mode:     {'ON' if args.alert else 'OFF'}")
    print("=" * 65)

    seq = 1
    base_temp = 24.0
    base_humidity = 45.0
    sent_count = 0

    try:
        while True:
            if args.fake_test:
                payload = {
                    "node_id": 0,
                    "seq": 0,
                    "type": "test",
                    "temp_c": 0.0,
                    "alert": False,
                    "verdict": "test",
                }
                print(f"[TEST PACKET] Sending fake test packet...")
            else:
                if args.simulate_gap and seq == 5:
                    print(f"[SIMULATING GAP] Skipping sequence numbers 5, 6, 7 (simulated packet loss)")
                    seq = 8

                if args.alert:
                    temp = round(random.uniform(36.0, 39.5), 2)
                    alert = True
                else:
                    base_temp += random.uniform(-0.15, 0.15)
                    temp = round(max(20.0, min(33.0, base_temp)), 2)
                    alert = False

                base_humidity += random.uniform(-0.5, 0.5)
                humidity = round(max(20.0, min(80.0, base_humidity)), 1)

                rssi = random.randint(-65, -45)
                snr = round(random.uniform(7.5, 11.0), 1)

                payload = {
                    "node_id": args.node,
                    "seq": seq,
                    "type": "reading",
                    "temp_c": temp,
                    "humidity_pct": humidity,
                    "alert": alert,
                    "rssi_dbm": rssi,
                    "snr_db": snr,
                    "verdict": "unverified",
                }

                print(
                    f"[TX] Node={args.node} SEQ={seq:04d} | Temp={temp:5.2f}°C | "
                    f"Hum={humidity:5.1f}% | Alert={'YES' if alert else 'NO '} | "
                    f"RSSI={rssi}dBm SNR={snr}dB"
                )

            t0 = time.time()
            try:
                resp = requests.post(url, headers=headers, json=payload, timeout=4, verify=verify_ca)
                latency_ms = (time.time() - t0) * 1000.0
                if resp.status_code == 200:
                    print(f"     -> POST OK (200) in {latency_ms:.1f}ms: {resp.text}")
                else:
                    print(f"     -> POST FAILED ({resp.status_code}): {resp.text}")
            except Exception as e:
                print(f"     -> POST ERROR: {e}")

            seq += 1
            sent_count += 1
            if args.count > 0 and sent_count >= args.count:
                print(f"\nCompleted sending {sent_count} packets.")
                break

            time.sleep(args.interval)

    except KeyboardInterrupt:
        print("\nSimulator stopped by user.")


if __name__ == "__main__":
    main()
