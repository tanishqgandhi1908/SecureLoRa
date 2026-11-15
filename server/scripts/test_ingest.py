#!/usr/bin/env python3
"""
test_ingest.py - Comprehensive verification script for SecureLoRa ingest endpoint.

Tests:
1. Valid reading with valid Bearer token -> 200 OK
2. Test reading (gateway 'fake' command) -> 200 OK
3. Missing Authorization header -> 401 Unauthorized
4. Invalid Bearer token -> 401 Unauthorized
5. Malformed JSON -> 400 Bad Request
6. Missing required fields -> 400 Bad Request
7. Invalid field types -> 400 Bad Request
8. High temperature alert packet -> 200 OK
9. Sequence gap tracking
10. Dual protocol verification (HTTP & HTTPS)
"""

import json
import sys
import os
import requests
from pathlib import Path

# Load settings from .env if present
script_dir = Path(__file__).resolve().parent
server_dir = script_dir.parent
env_file = server_dir / ".env"

token = "securelora_gateway_write_token_2026_demo"
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

HTTP_BASE = f"http://localhost:{http_port}"
HTTPS_BASE = f"https://localhost:{https_port}"
CA_CERT = str(server_dir / "certs" / "ca.pem")

PASSED = 0
FAILED = 0


def record_result(test_name: str, success: bool, detail: str = ""):
    global PASSED, FAILED
    if success:
        PASSED += 1
        print(f"  [PASS] {test_name} {detail}")
    else:
        FAILED += 1
        print(f"  [FAIL] {test_name} {detail}")


def run_tests():
    global PASSED, FAILED
    print("=" * 70)
    print("SecureLoRa Server Test Suite")
    print(f"HTTP Endpoint:  {HTTP_BASE}/api/readings")
    print(f"HTTPS Endpoint: {HTTPS_BASE}/api/readings")
    print(f"Using Token:    {token[:4]}...{token[-4:]}")
    print("=" * 70)

    # 1. Health Check
    try:
        r = requests.get(f"{HTTP_BASE}/health", timeout=3)
        record_result("Service Health Check", r.status_code == 200, f"(Status: {r.status_code})")
    except Exception as e:
        record_result("Service Health Check", False, f"Connection failed: {e}")
        print("\nEnsure Docker containers are running (docker compose up -d) before testing.")
        return 1

    headers_auth = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {token}",
    }

    # 2. Valid Reading (Step 1 contract)
    valid_reading = {
        "node_id": 1,
        "seq": 10,
        "type": "reading",
        "temp_c": 23.41,
        "pressure_hpa": 1012.6,
        "alert": False,
        "rssi_dbm": -45,
        "snr_db": 9.8,
        "verdict": "unverified",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=valid_reading)
    record_result("POST valid reading (200 OK)", r.status_code == 200, f"(got {r.status_code})")

    # 3. Test Message (gateway 'fake' command)
    test_reading = {
        "node_id": 0,
        "seq": 0,
        "type": "test",
        "temp_c": 0.0,
        "pressure_hpa": 0.0,
        "alert": False,
        "verdict": "test",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=test_reading)
    record_result("POST test reading (type='test') (200 OK)", r.status_code == 200, f"(got {r.status_code})")

    # 4. Missing Authorization Header (Step 2 security)
    headers_no_auth = {"Content-Type": "application/json"}
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_no_auth, json=valid_reading)
    record_result("Missing Authorization header (401 Unauthorized)", r.status_code == 401, f"(got {r.status_code})")

    # 5. Invalid Bearer Token (Step 2 security)
    headers_bad_token = {
        "Content-Type": "application/json",
        "Authorization": "Bearer wrong_token_xyz_123",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_bad_token, json=valid_reading)
    record_result("Invalid Bearer token (401 Unauthorized)", r.status_code == 401, f"(got {r.status_code})")

    # 6. Malformed JSON Body (Step 1 contract)
    r = requests.post(
        f"{HTTP_BASE}/api/readings",
        headers=headers_auth,
        data="this is not valid json {",
    )
    record_result("Malformed JSON syntax (400 Bad Request)", r.status_code == 400, f"(got {r.status_code})")

    # 7. Missing Required Field (e.g. missing 'temp_c')
    incomplete_reading = {
        "node_id": 1,
        "seq": 11,
        "type": "reading",
        # temp_c missing
        "pressure_hpa": 1012.6,
        "alert": False,
        "verdict": "unverified",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=incomplete_reading)
    record_result("Missing required field (400 Bad Request)", r.status_code == 400, f"(got {r.status_code})")

    # 8. Invalid Field Types (e.g. string for numeric seq)
    invalid_type_reading = {
        "node_id": 1,
        "seq": "not-an-integer",
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1012.6,
        "alert": False,
        "verdict": "unverified",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=invalid_type_reading)
    record_result("Invalid field types (400 Bad Request)", r.status_code == 400, f"(got {r.status_code})")

    # 9. High Temperature Alert Packet (>35 C)
    alert_reading = {
        "node_id": 1,
        "seq": 12,
        "type": "reading",
        "temp_c": 37.8,
        "pressure_hpa": 1010.2,
        "alert": True,
        "rssi_dbm": -42,
        "snr_db": 10.1,
        "verdict": "unverified",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=alert_reading)
    record_result("High temp alert packet (200 OK)", r.status_code == 200, f"(got {r.status_code})")

    # 10. Sequence Gap Detection
    gap_reading = {
        "node_id": 1,
        "seq": 17,  # Jumped from 12 to 17 (4 missed packets)
        "type": "reading",
        "temp_c": 28.0,
        "pressure_hpa": 1012.0,
        "alert": False,
        "rssi_dbm": -48,
        "snr_db": 9.2,
        "verdict": "unverified",
    }
    r = requests.post(f"{HTTP_BASE}/api/readings", headers=headers_auth, json=gap_reading)
    record_result("Sequence gap detection (200 OK)", r.status_code == 200, f"(got {r.status_code})")

    # 11. HTTPS Testing (with CA Certificate verification)
    if os.path.exists(CA_CERT):
        try:
            r = requests.post(
                f"{HTTPS_BASE}/api/readings",
                headers=headers_auth,
                json=valid_reading,
                verify=CA_CERT,
                timeout=3,
            )
            record_result("HTTPS Ingest with CA certificate (200 OK)", r.status_code == 200, f"(got {r.status_code})")
        except Exception as e:
            record_result("HTTPS Ingest with CA certificate", False, f"Error: {e}")
    else:
        print(f"  [SKIP] HTTPS test (CA cert not found at {CA_CERT})")

    # 12. Node Health Metrics Inspection
    r = requests.get(f"{HTTP_BASE}/api/nodes", headers=headers_auth)
    if r.status_code == 200:
        data = r.json()
        record_result("Node Health State API (200 OK)", True, f"(Active nodes tracked: {len(data.get('nodes', []))})")
    else:
        record_result("Node Health State API", False, f"(got {r.status_code})")

    print("=" * 70)
    print(f"Test Summary: {PASSED} Passed, {FAILED} Failed")
    print("=" * 70)
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(run_tests())
