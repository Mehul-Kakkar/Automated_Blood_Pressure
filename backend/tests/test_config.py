from pydantic import SecretStr, ValidationError
import pytest

from app.core.config import Settings


def test_settings_parse_frontend_origins_and_keep_database_url_secret() -> None:
    settings = Settings(
        _env_file=None,
        cors_origins="http://localhost:5173, https://app.example.test ",
        database_url="postgresql+psycopg://user:password@localhost/device",
    )

    assert settings.allowed_origins == ["http://localhost:5173", "https://app.example.test"]
    assert isinstance(settings.database_url, SecretStr)
    assert "password" not in str(settings.database_url)
    assert settings.database_configured is True


def test_settings_validate_database_timeout_bounds() -> None:
    with pytest.raises(ValidationError):
        Settings(_env_file=None, database_connect_timeout_seconds=0)


def test_settings_support_run_without_database() -> None:
    settings = Settings(_env_file=None, database_url=None, cors_origins="")

    assert settings.database_configured is False
    assert settings.allowed_origins == []


def test_settings_accept_emqx_environment_names_and_require_tls(monkeypatch) -> None:
    monkeypatch.setenv("MQTT_ENABLED", "true")
    monkeypatch.setenv("MQTT_BROKER_HOST", "k7128701.ala.asia-southeast1.emqxsl.com")
    monkeypatch.setenv("MQTT_BROKER_PORT", "8883")
    monkeypatch.setenv("MQTT_TLS_ENABLED", "true")
    monkeypatch.setenv("MQTT_USERNAME", "bp_backend")
    monkeypatch.setenv("MQTT_PASSWORD", "test-secret-not-real")
    monkeypatch.setenv("DATABASE_URL", "postgresql+psycopg://test:test@localhost/test")
    settings = Settings(_env_file=None)

    assert settings.mqtt_host == "k7128701.ala.asia-southeast1.emqxsl.com"
    assert settings.mqtt_port == 8883
    assert settings.mqtt_keepalive == 60
    assert settings.mqtt_configured is True

    with pytest.raises(ValidationError, match="must use TLS"):
        Settings(_env_file=None, mqtt_enabled=True, mqtt_host="broker.example.test", mqtt_username="bp_backend",
                 mqtt_password="test-secret-not-real", database_url="postgresql+psycopg://test:test@localhost/test",
                 mqtt_tls_enabled=False)
