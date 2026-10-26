"""Alembic environment for configured PostgreSQL or explicit test URLs."""

from logging.config import fileConfig

from alembic import context
from sqlalchemy import create_engine, pool

from app.core.config import get_settings
from app.db.base import Base
from app.db.session import create_database_engine
from app import models  # noqa: F401 - register mapped tables in Base.metadata

config = context.config
if config.config_file_name is not None:
    fileConfig(config.config_file_name)

target_metadata = Base.metadata


def configured_url() -> str:
    explicit_url = config.get_main_option("sqlalchemy.url")
    if explicit_url:
        return explicit_url
    settings = get_settings()
    if not settings.database_configured:
        raise RuntimeError("Set DATABASE_URL before running Alembic migrations.")
    assert settings.database_url is not None
    return settings.database_url.get_secret_value()


def run_migrations_offline() -> None:
    context.configure(
        url=configured_url(),
        target_metadata=target_metadata,
        literal_binds=True,
        dialect_opts={"paramstyle": "named"},
        compare_type=True,
    )
    with context.begin_transaction():
        context.run_migrations()


def run_migrations_online() -> None:
    supplied_connection = config.attributes.get("connection")
    if supplied_connection is not None:
        context.configure(connection=supplied_connection, target_metadata=target_metadata, compare_type=True)
        with context.begin_transaction():
            context.run_migrations()
        return

    url = configured_url()
    if url.startswith("postgresql"):
        engine = create_database_engine(get_settings()) if not config.get_main_option("sqlalchemy.url") else create_engine(
            url, poolclass=pool.NullPool, connect_args={"connect_timeout": get_settings().database_connect_timeout_seconds}
        )
    else:
        engine = create_engine(url, poolclass=pool.NullPool)
    with engine.connect() as connection:
        context.configure(connection=connection, target_metadata=target_metadata, compare_type=True)
        with context.begin_transaction():
            context.run_migrations()
    engine.dispose()


if context.is_offline_mode():
    run_migrations_offline()
else:
    run_migrations_online()
