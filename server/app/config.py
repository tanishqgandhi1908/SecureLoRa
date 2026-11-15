import os
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    """SecureLoRa server configuration settings."""

    # Gateway Ingest Authentication
    gateway_api_token: str = "securelora_gateway_write_token_2026_demo"

    # InfluxDB 2.x Settings
    influxdb_url: str = "http://influxdb:8086"
    influxdb_token: str = "securelora_influx_super_secret_token_2026_abcdef"
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
