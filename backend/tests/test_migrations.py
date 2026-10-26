from pathlib import Path

from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, inspect


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
    assert {"owner_id", "state_reported_at", "pressure_calibration_confirmed"}.issubset(device_columns)
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
