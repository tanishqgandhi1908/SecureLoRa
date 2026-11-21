import os
from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """SecureLoRa server configuration settings."""

    # Secrets have NO defaults: they must come from server/.env (or the environment).
    # min_length=16 makes the server refuse to start with an empty or trivially short token,
    # so authentication can never be switched off by accident.
    gateway_api_token: str = Field(..., min_length=16)

    # InfluxDB 2.x Settings
    influxdb_url: str = "http://influxdb:8086"
    influxdb_token: str = Field(..., min_length=16)
    influxdb_org: str = "securelora"
    influxdb_bucket: str = "readings"

    # Health & Logging
    log_dir: str = "./logs"
    stale_threshold_seconds: int = 180  # 3 minutes = 3 missed 60s heartbeats

    # Server port (internal)
    port: int = 8000

    model_config = SettingsConfigDict(
        env_file=os.environ.get("ENV_FILE", ".env"),
        env_file_encoding="utf-8",
        extra="ignore",
    )


settings = Settings()
