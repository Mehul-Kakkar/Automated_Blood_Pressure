"""Environment-backed application settings."""

from functools import lru_cache

from pydantic import Field, SecretStr, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    app_name: str = "Experimental Device API"
    environment: str = "development"
    log_level: str = "INFO"
    docs_enabled: bool = True
    cors_origins: str = ""
    database_url: SecretStr | None = None
    database_connect_timeout_seconds: int = Field(default=2, ge=1, le=30)
    max_telemetry_batch_samples: int = Field(default=500, ge=1, le=500)
    max_telemetry_batch_bytes: int = Field(default=262144, ge=1024, le=1048576)
    mqtt_enabled: bool = False
    mqtt_host: str | None = None
    mqtt_port: int = Field(default=8883, ge=1, le=65535)
    mqtt_client_id: str = Field(default="bp-backend", min_length=1, max_length=128)
    mqtt_username: str | None = None
    mqtt_password: SecretStr | None = None
    mqtt_ca_file: str | None = None
    mqtt_client_cert_file: str | None = None
    mqtt_client_key_file: str | None = None
    mqtt_queue_size: int = Field(default=500, ge=1, le=5000)
    mqtt_max_payload_bytes: int = Field(default=262144, ge=1024, le=1048576)
    mqtt_publish_timeout_seconds: int = Field(default=5, ge=1, le=30)
    mqtt_command_ttl_seconds: int = Field(default=30, ge=1, le=300)
    device_state_max_age_seconds: int = Field(default=30, ge=5, le=300)

    @model_validator(mode="after")
    def mqtt_requires_service_credentials(self) -> "Settings":
        if self.mqtt_enabled and not (self.mqtt_host and self.mqtt_username and self.mqtt_password):
            raise ValueError("MQTT_ENABLED requires MQTT_HOST, MQTT_USERNAME, and MQTT_PASSWORD")
        if self.mqtt_enabled and not self.database_configured:
            raise ValueError("MQTT_ENABLED requires DATABASE_URL for device enrollment and telemetry persistence")
        if bool(self.mqtt_client_cert_file) != bool(self.mqtt_client_key_file):
            raise ValueError("MQTT_CLIENT_CERT_FILE and MQTT_CLIENT_KEY_FILE must be configured together")
        return self

    @property
    def mqtt_configured(self) -> bool:
        return self.mqtt_enabled and bool(self.mqtt_host and self.mqtt_username and self.mqtt_password)

    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        case_sensitive=False,
        extra="ignore",
    )

    @property
    def allowed_origins(self) -> list[str]:
        """Return explicitly configured origins; an empty setting disables CORS."""
        return [origin.strip() for origin in self.cors_origins.split(",") if origin.strip()]

    @property
    def database_configured(self) -> bool:
        return self.database_url is not None and bool(self.database_url.get_secret_value().strip())


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    """Return cached settings for the process; tests can clear this cache."""
    return Settings()
