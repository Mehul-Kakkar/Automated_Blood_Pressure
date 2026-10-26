"""Opt-in integration checks for a dedicated PostgreSQL test database."""

import os
from uuid import uuid4

import pytest
from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, select
from sqlalchemy.engine import Engine, make_url
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models import Device, MeasurementSession, TelemetryBatch
from app.services.telemetry import ingest_telemetry_batch
from tests.test_persistence import make_payload


@pytest.fixture(scope="session")
def postgres_engine() -> Engine:
    url = os.getenv("TEST_POSTGRES_URL")
    if not url:
        pytest.skip("Set TEST_POSTGRES_URL to a dedicated disposable PostgreSQL database.")
    parsed_url = make_url(url)
    if parsed_url.get_backend_name() != "postgresql":
        pytest.fail("TEST_POSTGRES_URL must use PostgreSQL.")
    database_name = parsed_url.database or ""
    if "test" not in database_name.lower():
        pytest.fail("Refusing to run migrations unless the database name contains 'test'.")

    engine = create_engine(url, pool_pre_ping=True)
    config = Config("alembic.ini")
    with engine.begin() as connection:
        config.attributes["connection"] = connection
        command.upgrade(config, "head")
        command.check(config)
    yield engine
    engine.dispose()


def _create_device_and_session(db: Session) -> tuple[Device, MeasurementSession]:
    device = Device(device_identifier=f"integration-test-{uuid4()}")
    db.add(device)
    db.flush()
    measurement_session = MeasurementSession(
        device_id=device.id,
        lifecycle="created",
        data_provenance="integration_test",
    )
    db.add(measurement_session)
    db.commit()
    return device, measurement_session


def _cleanup(db: Session, device: Device, measurement_session: MeasurementSession) -> None:
    db.delete(measurement_session)
    db.flush()
    db.delete(device)
    db.commit()


def test_postgres_migration_and_batch_upload_idempotency(postgres_engine: Engine) -> None:
    with Session(postgres_engine, expire_on_commit=False) as db:
        device, measurement_session = _create_device_and_session(db)
        try:
            payload = make_payload(device.id, measurement_session.id)
            first = ingest_telemetry_batch(db, payload)
            repeated = ingest_telemetry_batch(db, payload)

            assert first.duplicate is False
            assert repeated.duplicate is True
            assert db.scalar(select(TelemetryBatch).where(TelemetryBatch.batch_id == payload.batch_id)) is not None
        finally:
            _cleanup(db, device, measurement_session)


def test_postgres_enforces_batch_count_constraint(postgres_engine: Engine) -> None:
    with Session(postgres_engine, expire_on_commit=False) as db:
        device, measurement_session = _create_device_and_session(db)
        try:
            db.add(
                TelemetryBatch(
                    batch_id=uuid4(),
                    device_id=device.id,
                    session_id=measurement_session.id,
                    payload_sha256="b" * 64,
                    sample_count=0,
                    event_count=0,
                    schema_version="future-v1",
                    data_provenance="integration_test",
                )
            )
            with pytest.raises(IntegrityError):
                db.commit()
            db.rollback()
        finally:
            _cleanup(db, device, measurement_session)
