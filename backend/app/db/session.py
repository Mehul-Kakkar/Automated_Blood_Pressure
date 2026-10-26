"""SQLAlchemy engine and session dependencies."""

from collections.abc import Generator
from functools import lru_cache

from sqlalchemy import Engine, create_engine
from sqlalchemy.orm import Session, sessionmaker

from app.core.config import Settings, get_settings


class DatabaseNotConfiguredError(RuntimeError):
    """Raised when persistence is requested without DATABASE_URL."""


def create_database_engine(settings: Settings) -> Engine:
    if not settings.database_configured:
        raise DatabaseNotConfiguredError("DATABASE_URL is not configured.")
    assert settings.database_url is not None
    url = settings.database_url.get_secret_value()
    connect_args = {"connect_timeout": settings.database_connect_timeout_seconds} if url.startswith("postgresql") else {}
    return create_engine(url, pool_pre_ping=True, connect_args=connect_args)


@lru_cache(maxsize=1)
def get_engine() -> Engine:
    return create_database_engine(get_settings())


@lru_cache(maxsize=1)
def get_session_factory() -> sessionmaker[Session]:
    return sessionmaker(bind=get_engine(), autoflush=False, expire_on_commit=False)


def get_db_session() -> Generator[Session, None, None]:
    """FastAPI dependency that closes every request-scoped session."""
    factory = get_session_factory()
    with factory() as session:
        yield session
