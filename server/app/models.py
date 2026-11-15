from typing import Optional, Literal
from pydantic import BaseModel, Field, model_validator


class ReadingPayload(BaseModel):
    """
    Gateway payload schema for SecureLoRa readings and test packets.
    Strictly validated per Section 2 of server_guide.
    """
    node_id: int = Field(..., description="Sender node ID (e.g. 1; 0 = test)")
    seq: int = Field(..., description="Sequence counter from the node, +1 per packet")
    type: str = Field(..., description="'reading', 'test', 'heartbeat', or 'alert'")
    temp_c: float = Field(..., description="Temperature in degrees Celsius")
    pressure_hpa: float = Field(..., description="Atmospheric pressure in hPa")
    alert: bool = Field(..., description="High temperature alert flag (>35 C)")
    verdict: str = Field(
        ...,
        description="'unverified', 'test', 'accepted', 'bad_signature', 'replay', 'unknown_session'"
    )
    
    # Optional RF and metadata fields
    rssi_dbm: Optional[float] = Field(None, description="Radio signal strength in dBm")
    snr_db: Optional[float] = Field(None, description="Signal-to-noise ratio in dB")
    session_id: Optional[str] = Field(None, description="Session ID hex string")
    battery_mv: Optional[float] = Field(None, description="Node battery voltage in millivolts")
    reason: Optional[str] = Field(None, description="Rejection or event reason")
    gw_rx_time: Optional[float] = Field(None, description="Gateway receive timestamp")

    @model_validator(mode="after")
    def validate_reading_fields(self) -> "ReadingPayload":
        # In real readings, rssi_dbm and snr_db are typically provided by gateway
        # but test messages (from fake command) do not have them.
        if self.type == "test":
            if self.node_id != 0 and self.node_id != 1:
                # allow test from any node, but standard is 0
                pass
        return self


class IngestResponse(BaseModel):
    """Standard success response returned to the gateway."""
    status: Literal["ok"] = "ok"
    node_id: int
    seq: int
    verdict: str
    server_rx_time: float


class ErrorResponse(BaseModel):
    """Standard error response."""
    error: str
    details: Optional[dict | list | str] = None
