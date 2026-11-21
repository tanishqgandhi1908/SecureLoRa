import json
import logging
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from app.config import settings

# Standard console logger setup
console_handler = logging.StreamHandler(sys.stdout)
console_handler.setFormatter(
    logging.Formatter("[%(asctime)s] [%(levelname)s] [%(name)s]: %(message)s", "%Y-%m-%d %H:%M:%S")
)

logger = logging.getLogger("securelora")
logger.setLevel(logging.INFO)
logger.addHandler(console_handler)


class IngestAuditLogger:
    """
    Appends every request, verdict, and error as structured JSON Lines (jsonl).
    Essential for the 72-hour reliability analysis and gap reporting.
    """

    def __init__(self, log_dir: str):
        self.log_dir = Path(log_dir)
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.log_file = self.log_dir / "ingest.jsonl"

    def log_event(
        self,
        client_ip: str,
        status_code: int,
        duration_ms: float,
        server_rx_time: float,
        endpoint: str = "/api/readings",
        payload: Optional[dict[str, Any]] = None,
        error: Optional[str] = None,
    ) -> None:
        record = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "server_rx_time": server_rx_time,
            "client_ip": client_ip,
            "endpoint": endpoint,
            "status_code": status_code,
            "duration_ms": round(duration_ms, 3),
            "payload": payload,
            "error": error,
        }

        # Convenience flattened fields for fast jq / python parsing
        if payload:
            record["node_id"] = payload.get("node_id")
            record["seq"] = payload.get("seq")
            record["type"] = payload.get("type")
            record["verdict"] = payload.get("verdict")
            record["temp_c"] = payload.get("temp_c")
            record["alert"] = payload.get("alert")
            record["rssi_dbm"] = payload.get("rssi_dbm")
            record["snr_db"] = payload.get("snr_db")

        line = json.dumps(record) + "\n"
        try:
            with open(self.log_file, "a", encoding="utf-8") as f:
                f.write(line)
                f.flush()
        except Exception as e:
            logger.error(f"Failed to write audit log to {self.log_file}: {e}")


audit_logger = IngestAuditLogger(settings.log_dir)
