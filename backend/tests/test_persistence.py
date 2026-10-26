from datetime import datetime, timezone
from decimal import Decimal
from uuid import uuid4

import pytest
from pydantic import ValidationError
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models import AcousticEvent, Device, MeasurementSession, Report, TelemetryBatch, TelemetrySample
from app.schemas.telemetry import TelemetryBatchInput
from app.services.telemetry import (
    TelemetryBatchConflictError,
    TelemetryIngestError,
    TelemetryPayloadTooLargeError,
    ingest_telemetry_batch,
)


def make_payload(
    device_id, session_id, *, batch_id=None, sample_id=None, pressure=121.5, sample_sequence=42
) -> TelemetryBatchInput:
    return TelemetryBatchInput.model_validate(
        {
            "batch_id": str(batch_id or uuid4()),
            "device_id": str(device_id),
            "session_id": str(session_id),
            "schema_version": "future-v1",
            "firmware_version": None,
            "processing_algorithm_version": "detector-test",
            "data_provenance": "device_live",
            "samples": [
                {
                    "sample_id": str(sample_id or uuid4()),
                    "sample_sequence": sample_sequence,
                    "device_timestamp_ms": 9000,
                    "device_recorded_at": "2026-10-26T12:00:00Z",
                    "pressure_mmhg": pressure,
                    "heart_rate_bpm": 72,
                    "raw_adc": 2048,
                    "filtered": -2.4,
                    "acoustic_envelope": 1.7,
                    "noise_floor": 3,
                    "threshold": 12,
                    "event_detected": False,
                    "is_valid": True,
                    "quality_flags": [],
                }
            ],
            "acoustic_events": [
                {
                    "event_id": str(uuid4()),
                    "event_sequence": None,
                    "event_type": "acoustic_detected",
                    "device_timestamp_ms": 8500,
                    "strength": 2.3,
                    "pressure_mmhg": None,
                    "quality_flags": [],
                }
            ],
        }
    )


def create_device_and_session(db: Session) -> tuple[Device, MeasurementSession]:
    device = Device(device_identifier=f"test-device-{uuid4()}", firmware_version=None)
    db.add(device)
    db.flush()
    session = MeasurementSession(
        device_id=device.id,
        lifecycle="created",
        data_provenance="test_fixture",
        created_at=datetime.now(timezone.utc),
    )
    db.add(session)
    db.commit()
    return device, session


def test_batch_insert_is_atomic_and_repeated_upload_is_idempotent(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    payload = make_payload(device.id, session.id)

    first = ingest_telemetry_batch(db_session, payload)
    second = ingest_telemetry_batch(db_session, payload)

    assert first.duplicate is False
    assert second.duplicate is True
    assert first.batch_id == second.batch_id
    assert db_session.scalar(select(TelemetryBatch).where(TelemetryBatch.device_id == device.id)) is not None
    assert len(db_session.scalars(select(TelemetrySample)).all()) == 1
    assert len(db_session.scalars(select(AcousticEvent)).all()) == 1


def test_reusing_batch_id_with_changed_payload_conflicts_without_mutation(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    batch_id = uuid4()
    ingest_telemetry_batch(db_session, make_payload(device.id, session.id, batch_id=batch_id, pressure=120))

    with pytest.raises(TelemetryBatchConflictError):
        ingest_telemetry_batch(db_session, make_payload(device.id, session.id, batch_id=batch_id, pressure=121))

    assert len(db_session.scalars(select(TelemetrySample)).all()) == 1


def test_duplicate_sample_id_across_batches_is_rejected_atomically(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    sample_id = uuid4()
    ingest_telemetry_batch(db_session, make_payload(device.id, session.id, sample_id=sample_id))

    with pytest.raises(TelemetryBatchConflictError):
        ingest_telemetry_batch(
            db_session, make_payload(device.id, session.id, sample_id=sample_id, sample_sequence=43)
        )

    assert len(db_session.scalars(select(TelemetryBatch)).all()) == 1
    assert len(db_session.scalars(select(TelemetrySample)).all()) == 1


def test_wrong_device_session_pair_is_rejected(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    other_device, _other_session = create_device_and_session(db_session)
    payload = make_payload(other_device.id, session.id)

    with pytest.raises(TelemetryIngestError):
        ingest_telemetry_batch(db_session, payload)

    assert db_session.scalar(select(TelemetryBatch)) is None


def test_configured_batch_limits_are_checked_before_database_write(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    payload = make_payload(device.id, session.id)

    with pytest.raises(TelemetryPayloadTooLargeError):
        ingest_telemetry_batch(db_session, payload, max_samples=0)
    with pytest.raises(TelemetryPayloadTooLargeError):
        ingest_telemetry_batch(db_session, payload, max_bytes=64)

    assert db_session.scalar(select(TelemetryBatch)) is None


@pytest.mark.parametrize(
    "override",
    [
        {"pressure_mmhg": -1},
        {"pressure_mmhg": 401},
        {"heart_rate_bpm": 0},
        {"raw_adc": 4096},
        {"device_recorded_at": "2026-10-26T12:00:00"},
        {"unexpected_field": "not accepted"},
    ],
)
def test_invalid_sample_fields_are_rejected(override: dict[str, object]) -> None:
    sample = {
        "sample_id": str(uuid4()),
        "pressure_mmhg": 120,
        "heart_rate_bpm": 72,
        "device_recorded_at": "2026-10-26T12:00:00Z",
    }
    sample.update(override)
    with pytest.raises(ValidationError):
        TelemetryBatchInput.model_validate(
            {
                "batch_id": str(uuid4()),
                "device_id": str(uuid4()),
                "session_id": str(uuid4()),
                "schema_version": "future-v1",
                "data_provenance": "device_live",
                "samples": [sample],
            }
        )


def test_batch_limits_and_duplicate_identifiers_are_enforced() -> None:
    sample_id = str(uuid4())
    base = {
        "batch_id": str(uuid4()),
        "device_id": str(uuid4()),
        "session_id": str(uuid4()),
        "schema_version": "future-v1",
        "data_provenance": "device_live",
        "samples": [{"sample_id": sample_id}, {"sample_id": sample_id}],
    }
    with pytest.raises(ValidationError, match="unique"):
        TelemetryBatchInput.model_validate(base)


def test_database_constraints_reject_invalid_session_and_report_states(db_session: Session) -> None:
    device, _ = create_device_and_session(db_session)
    db_session.add(
        MeasurementSession(device_id=device.id, lifecycle="done", data_provenance="test_fixture")
    )
    with pytest.raises(IntegrityError):
        db_session.commit()
    db_session.rollback()


def test_completed_session_requires_confirmed_start_and_end_timestamps(db_session: Session) -> None:
    device, _ = create_device_and_session(db_session)
    db_session.add(
        MeasurementSession(device_id=device.id, lifecycle="completed", data_provenance="test_fixture")
    )
    with pytest.raises(IntegrityError):
        db_session.commit()
    db_session.rollback()


def test_pressure_database_constraint_requires_explicit_mmhg_unit(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    batch = TelemetryBatch(
        batch_id=uuid4(), device_id=device.id, session_id=session.id, payload_sha256="a" * 64,
        sample_count=1, event_count=0, schema_version="future-v1", data_provenance="test_fixture",
    )
    db_session.add(batch)
    db_session.flush()
    db_session.add(
        TelemetrySample(
            sample_id=uuid4(), batch_row_id=batch.id, device_id=device.id, session_id=session.id,
            pressure_mmhg=Decimal("120"), pressure_unit=None, is_valid=True,
            quality_flags=[], data_provenance="test_fixture", synchronization_metadata={},
        )
    )
    with pytest.raises(IntegrityError):
        db_session.commit()
    db_session.rollback()


def test_report_metadata_must_reference_session_and_valid_status(db_session: Session) -> None:
    device, session = create_device_and_session(db_session)
    report = Report(
        session_id=session.id,
        report_version="v1",
        generator_version="none-yet",
        status="complete",
    )
    db_session.add(report)
    with pytest.raises(IntegrityError):
        db_session.commit()
    db_session.rollback()
