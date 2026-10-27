from pathlib import Path
from uuid import uuid4

from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, inspect, text


def test_initial_migration_up_and_down_against_temporary_sqlite(tmp_path: Path) -> None:
    database_path = tmp_path / "migration-test.db"
    config = Config("alembic.ini")
    config.set_main_option("sqlalchemy.url", f"sqlite:///{database_path.as_posix()}")

    command.upgrade(config, "head")
    command.check(config)

    engine = create_engine(f"sqlite:///{database_path.as_posix()}")
    tables = set(inspect(engine).get_table_names())
    assert {
        "devices",
        "measurement_sessions",
        "telemetry_batches",
        "telemetry_samples",
        "acoustic_events",
        "reports",
        "owners",
        "owner_credentials",
        "session_events",
        "alembic_version",
    }.issubset(tables)
    device_columns = {column["name"] for column in inspect(engine).get_columns("devices")}
    session_columns = {column["name"] for column in inspect(engine).get_columns("measurement_sessions")}
    assert {"owner_username", "state_reported_at", "pressure_calibration_confirmed"}.issubset(device_columns)
    owner_columns = {column["name"] for column in inspect(engine).get_columns("owners")}
    assert {"username", "legacy_id", "display_name", "avatar_data", "password_hash"}.issubset(owner_columns)
    assert "owner_username" in {column["name"] for column in inspect(engine).get_columns("owner_credentials")}
    assert "client_request_id" in session_columns
    report_columns = {column["name"] for column in inspect(engine).get_columns("reports")}
    assert {"report_data", "content_sha256"}.issubset(report_columns)
    engine.dispose()
    command.downgrade(config, "base")
    engine = create_engine(f"sqlite:///{database_path.as_posix()}")
    tables_after_downgrade = set(inspect(engine).get_table_names())
    assert "devices" not in tables_after_downgrade
    assert "measurement_sessions" not in tables_after_downgrade
    engine.dispose()


def test_username_migration_preserves_existing_owner_and_relationships(tmp_path: Path) -> None:
    database_path = tmp_path / "owner-migration.db"
    url = f"sqlite:///{database_path.as_posix()}"
    config = Config("alembic.ini")
    config.set_main_option("sqlalchemy.url", url)
    command.upgrade(config, "0004_report_snapshot")
    owner_id, credential_id, device_id = str(uuid4()), str(uuid4()), str(uuid4())
    engine = create_engine(url)
    with engine.begin() as connection:
        connection.execute(text("INSERT INTO owners (id, display_name) VALUES (:id, :name)"),
                           {"id": owner_id, "name": "Existing Display Name"})
        connection.execute(text("INSERT INTO owner_credentials (id, owner_id, token_sha256) VALUES (:id, :owner, :hash)"),
                           {"id": credential_id, "owner": owner_id, "hash": "a" * 64})
        connection.execute(text("INSERT INTO devices (id, device_identifier, owner_id) VALUES (:id, :identifier, :owner)"),
                           {"id": device_id, "identifier": "existing-device", "owner": owner_id})
    command.upgrade(config, "head")
    username = f"user_{owner_id.replace('-', '')}"
    with engine.connect() as connection:
        owner = connection.execute(text("SELECT username, display_name, legacy_id FROM owners")).one()
        assert owner == (username, "Existing Display Name", owner_id)
        assert connection.execute(text("SELECT owner_username FROM owner_credentials WHERE id=:id"),
                                  {"id": credential_id}).scalar_one() == username
        assert connection.execute(text("SELECT owner_username FROM devices WHERE id=:id"),
                                  {"id": device_id}).scalar_one() == username
    command.downgrade(config, "0004_report_snapshot")
    with engine.connect() as connection:
        assert connection.execute(text("SELECT id, display_name FROM owners")).one() == (owner_id, "Existing Display Name")
        assert connection.execute(text("SELECT owner_id FROM owner_credentials WHERE id=:id"),
                                  {"id": credential_id}).scalar_one() == owner_id
        assert connection.execute(text("SELECT owner_id FROM devices WHERE id=:id"),
                                  {"id": device_id}).scalar_one() == owner_id
    engine.dispose()
