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
