import time
from datetime import datetime, timezone
from typing import Optional

from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS

from app.config import settings
from app.logger import logger
from app.models import ReadingPayload


class InfluxService:
    """Manages InfluxDB 2.x connection and point writing."""

    def __init__(self):
        self._client: Optional[InfluxDBClient] = None
        self._write_api = None
        self._is_connected = False
        self._init_client()

    def _init_client(self) -> None:
        try:
            self._client = InfluxDBClient(
                url=settings.influxdb_url,
                token=settings.influxdb_token,
                org=settings.influxdb_org,
                timeout=3000,  # 3 seconds max timeout to avoid gateway timing out
            )
            self._write_api = self._client.write_api(write_options=SYNCHRONOUS)
            self._is_connected = True
            logger.info(f"InfluxDB client initialized for {settings.influxdb_url} (org: {settings.influxdb_org})")
        except Exception as e:
            self._is_connected = False
            logger.error(f"Failed to initialize InfluxDB client: {e}")

    def is_healthy(self) -> bool:
        if not self._client:
            self._init_client()
        if not self._client:
            return False
        try:
            return bool(self._client.ping())
        except Exception as e:
            logger.debug(f"InfluxDB health check failed: {e}")
            return False

    def write_reading(
        self,
        payload: ReadingPayload,
        server_rx_time: float,
        delivery_rate_pct: Optional[float] = None,
        gap_seconds: Optional[float] = None,
    ) -> bool:
        """Writes reading and link health points to InfluxDB."""
        if not self._client or not self._write_api:
            self._init_client()
        if not self._write_api:
            logger.warning("InfluxDB write_api unavailable. Skipping point write.")
            return False

        points = []
        now_dt = datetime.fromtimestamp(server_rx_time, tz=timezone.utc)

        # 1. Measurement: reading
        p_reading = (
            Point("reading")
            .tag("node_id", str(payload.node_id))
            .tag("session_id", payload.session_id or "none")
            .tag("type", payload.type)
            .tag("verdict", payload.verdict)
            .tag("is_test", "true" if payload.type == "test" else "false")
            .field("temp_c", float(payload.temp_c))
            .field("alert", bool(payload.alert))
            .field("alert_int", 1 if payload.alert else 0)
            .field("seq", int(payload.seq))
            .field("server_rx_time", float(server_rx_time))
            .time(now_dt, WritePrecision.NS)
        )

        if payload.pressure_hpa is not None:
            p_reading.field("pressure_hpa", float(payload.pressure_hpa))
        if payload.humidity_pct is not None:
            p_reading.field("humidity_pct", float(payload.humidity_pct))
        if payload.rssi_dbm is not None:
            p_reading.field("rssi_dbm", float(payload.rssi_dbm))
        if payload.snr_db is not None:
            p_reading.field("snr_db", float(payload.snr_db))
        if payload.battery_mv is not None:
            p_reading.field("battery_mv", float(payload.battery_mv))
        if payload.gw_rx_time is not None:
            p_reading.field("gw_rx_time", float(payload.gw_rx_time))

        points.append(p_reading)

        # 2. Measurement: packet_event
        p_event = (
            Point("packet_event")
            .tag("verdict", payload.verdict)
            .tag("node_id", str(payload.node_id))
            .tag("type", payload.type)
            .field("seq", int(payload.seq))
            .field("server_rx_time", float(server_rx_time))
            .field("reason", payload.reason or "")
            .time(now_dt, WritePrecision.NS)
        )
        if payload.rssi_dbm is not None:
            p_event.field("rssi", float(payload.rssi_dbm))
        if payload.snr_db is not None:
            p_event.field("snr", float(payload.snr_db))
        points.append(p_event)

        # 3. Measurement: link_health (if gap_seconds / delivery_rate provided)
        if gap_seconds is not None:
            is_stale = gap_seconds > settings.stale_threshold_seconds
            p_health = (
                Point("link_health")
                .tag("node_id", str(payload.node_id))
                .field("seq", int(payload.seq))
                .field("gap_seconds", float(gap_seconds))
                .field("stale", is_stale)
                .field("stale_int", 1 if is_stale else 0)
                .time(now_dt, WritePrecision.NS)
            )
            if delivery_rate_pct is not None:
                p_health.field("delivery_rate_pct", float(delivery_rate_pct))
            points.append(p_health)

        try:
            self._write_api.write(bucket=settings.influxdb_bucket, org=settings.influxdb_org, record=points)
            return True
        except Exception as e:
            logger.error(f"InfluxDB write error: {e}")
            return False

    def write_stale_update(self, node_id: int, gap_seconds: float, stale: bool = True) -> bool:
        """Periodically writes staleness update if no packets have been received."""
        if not self._write_api:
            return False
        try:
            now_dt = datetime.now(timezone.utc)
            p = (
                Point("link_health")
                .tag("node_id", str(node_id))
                .field("gap_seconds", float(gap_seconds))
                .field("stale", stale)
                .field("stale_int", 1 if stale else 0)
                .time(now_dt, WritePrecision.NS)
            )
            self._write_api.write(bucket=settings.influxdb_bucket, org=settings.influxdb_org, record=p)
            return True
        except Exception as e:
            logger.error(f"Failed to write stale link_health update for node {node_id}: {e}")
            return False

    def close(self) -> None:
        if self._write_api:
            try:
                self._write_api.close()
            except Exception:
                pass
        if self._client:
            try:
                self._client.close()
            except Exception:
                pass


influx_service = InfluxService()
