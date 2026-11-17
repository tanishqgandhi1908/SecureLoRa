import pytest
from fastapi.testclient import TestClient

from app.config import settings
from app.link_health import LinkHealthTracker
from app.main import app

client = TestClient(app)

VALID_TOKEN = settings.gateway_api_token
AUTH_HEADER = {"Authorization": f"Bearer {VALID_TOKEN}"}


def test_health_endpoint():
    response = client.get("/health")
    assert response.status_code == 200
    data = response.json()
    assert "status" in data
    assert "uptime_seconds" in data


def test_post_valid_reading():
    payload = {
        "node_id": 1,
        "seq": 1,
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1013.2,
        "alert": False,
        "rssi_dbm": -55,
        "snr_db": 8.5,
        "verdict": "unverified",
    }
    response = client.post("/api/readings", headers=AUTH_HEADER, json=payload)
    assert response.status_code == 200
    data = response.json()
    assert data["status"] == "ok"
    assert data["node_id"] == 1
    assert data["seq"] == 1
    assert data["verdict"] == "unverified"
    assert "server_rx_time" in data


def test_post_test_reading():
    payload = {
        "node_id": 0,
        "seq": 0,
        "type": "test",
        "temp_c": 0.0,
        "pressure_hpa": 0.0,
        "alert": False,
        "verdict": "test",
    }
    response = client.post("/api/readings", headers=AUTH_HEADER, json=payload)
    assert response.status_code == 200
    data = response.json()
    assert data["status"] == "ok"
    assert data["node_id"] == 0
    assert data["type"] if "type" in data else True


def test_missing_auth_header():
    payload = {
        "node_id": 1,
        "seq": 2,
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1013.2,
        "alert": False,
        "verdict": "unverified",
    }
    response = client.post("/api/readings", json=payload)
    assert response.status_code == 401
    assert "detail" in response.json() or "error" in response.json()


def test_invalid_bearer_token():
    payload = {
        "node_id": 1,
        "seq": 2,
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1013.2,
        "alert": False,
        "verdict": "unverified",
    }
    headers = {"Authorization": "Bearer wrong_token"}
    response = client.post("/api/readings", headers=headers, json=payload)
    assert response.status_code == 401


def test_malformed_json_body():
    headers = {"Authorization": f"Bearer {VALID_TOKEN}", "Content-Type": "application/json"}
    response = client.post("/api/readings", headers=headers, content="{broken json")
    assert response.status_code == 400


def test_missing_required_field():
    payload = {
        "node_id": 1,
        # missing seq
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1013.2,
        "alert": False,
        "verdict": "unverified",
    }
    response = client.post("/api/readings", headers=AUTH_HEADER, json=payload)
    assert response.status_code == 400


def test_invalid_data_types():
    payload = {
        "node_id": 1,
        "seq": 3,
        "type": "reading",
        "temp_c": "very_hot",  # string instead of float
        "pressure_hpa": 1013.2,
        "alert": False,
        "verdict": "unverified",
    }
    response = client.post("/api/readings", headers=AUTH_HEADER, json=payload)
    assert response.status_code == 400


def test_link_health_tracker_gap_detection():
    tracker = LinkHealthTracker()
    t0 = 1000.0

    # First packet: seq 1
    s1 = tracker.record_packet(node_id=1, seq=1, rx_time=t0)
    assert s1.received_msgs == 1
    assert s1.missed_msgs == 0
    assert s1.delivery_rate_pct == 100.0

    # Second packet: seq 4 (missed seq 2 and 3 -> 2 missed)
    s2 = tracker.record_packet(node_id=1, seq=4, rx_time=t0 + 60.0)
    assert s2.received_msgs == 2
    assert s2.missed_msgs == 2
    assert s2.expected_msgs == 4
    assert s2.delivery_rate_pct == 50.0
    assert s2.last_gap_seconds == 60.0

    # Staleness check
    assert not s2.is_stale(now=t0 + 60.0 + 100.0)  # 100s < 180s -> not stale
    assert s2.is_stale(now=t0 + 60.0 + 200.0)      # 200s > 180s -> stale!


def test_storage_failure_returns_503(monkeypatch):
    """If InfluxDB can't save the reading, the server must not claim success (200)."""
    from app.influx import influx_service

    monkeypatch.setattr(influx_service, "write_reading", lambda *args, **kwargs: False)
    payload = {
        "node_id": 1,
        "seq": 50,
        "type": "reading",
        "temp_c": 24.5,
        "pressure_hpa": 1013.2,
        "alert": False,
        "verdict": "unverified",
    }
    response = client.post("/api/readings", headers=AUTH_HEADER, json=payload)
    assert response.status_code == 503


def test_settings_reject_missing_or_short_token(monkeypatch):
    """The server must refuse to start without a real gateway token (no auth bypass)."""
    import pydantic
    from app.config import Settings

    monkeypatch.setenv("GATEWAY_API_TOKEN", "")
    with pytest.raises(pydantic.ValidationError):
        Settings(_env_file=None)
    monkeypatch.setenv("GATEWAY_API_TOKEN", "CHANGE_ME")
    with pytest.raises(pydantic.ValidationError):
        Settings(_env_file=None)
