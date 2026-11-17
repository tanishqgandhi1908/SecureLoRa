import os

import pytest

# Test-only values so the app can start inside pytest. They are NOT used by the real server,
# which reads its secrets from server/.env and refuses to start without them.
os.environ.setdefault("GATEWAY_API_TOKEN", "pytest-only-gateway-token-0123456789")
os.environ.setdefault("INFLUXDB_TOKEN", "pytest-only-influx-token-0123456789")


@pytest.fixture(autouse=True)
def fake_influx_write(monkeypatch):
    """Unit tests don't need a running InfluxDB: pretend every write succeeds.
    A test can override this to simulate a storage failure."""
    from app.influx import influx_service

    monkeypatch.setattr(influx_service, "write_reading", lambda *args, **kwargs: True)
