"""Synthetic fixtures for deterministic report behavior; no clinical or device data."""
from datetime import datetime, timezone
from decimal import Decimal
from uuid import uuid4

from fastapi.testclient import TestClient
from sqlalchemy.orm import sessionmaker

from app.db.session import get_db_session
from app.models import (AcousticEvent, Device, MeasurementSession, Owner, SessionEvent,
                        TelemetryBatch, TelemetrySample)
from app.security.auth import get_current_owner_username
from app.services.reports import ReportService, build_report_data, generate_pdf


def make_session(db_session, *, completed: bool = False):
    owner = Owner(username="synthetic_report_test_owner", display_name="synthetic report test owner")
    db_session.add(owner)
    db_session.flush()
    device = Device(owner_username=owner.username, device_identifier=f"synthetic-{uuid4()}", firmware_version="fixture-fw",
                    processing_algorithm_version="fixture-algo")
    db_session.add(device)
    db_session.flush()
    start = datetime.now(timezone.utc) if completed else None
    session = MeasurementSession(device_id=device.id, lifecycle="completed" if completed else "created",
                                 data_provenance="synthetic_test", started_at=start, ended_at=start)
    db_session.add(session)
    db_session.flush()
    batch = TelemetryBatch(batch_id=uuid4(), device_id=device.id, session_id=session.id,
        payload_sha256="0" * 64, sample_count=1, event_count=0, schema_version="test",
        data_provenance="synthetic_test")
    db_session.add(batch)
    db_session.flush()
    return owner, device, session, batch


def test_empty_session_creates_deterministic_uninterpretable_snapshot(db_session) -> None:
    _owner, device, session, _batch = make_session(db_session)
    report, created = ReportService(db_session).get_or_create(session, device)
    assert created is True
    assert report.report_data["interpretability"]["status"] == "uninterpretable"
    assert report.report_data["derived_measurements"]["systolic_mmhg"] is None
    assert report.report_data["pressure_trace"]["samples"] == []
    repeated, created = ReportService(db_session).get_or_create(session, device)
    assert created is False
    assert repeated.id == report.id
    assert repeated.content_sha256 == report.content_sha256


def test_incomplete_session_and_invalid_sample_are_explicit(db_session) -> None:
    _owner, device, session, batch = make_session(db_session)
    db_session.add(TelemetrySample(sample_id=uuid4(), batch_row_id=batch.id, device_id=device.id,
        session_id=session.id, raw_adc=1200, acoustic_envelope=0.5, is_valid=False,
        quality_flags=["fixture_invalid"], data_provenance="synthetic_test"))
    db_session.commit()
    report, _ = ReportService(db_session).get_or_create(session, device)
    data = report.report_data
    assert data["session"]["lifecycle"] == "created"
    assert data["interpretability"]["status"] == "uninterpretable"
    assert data["pressure_trace"]["samples"] == []
    assert data["heart_rate"]["summary"] is None
    assert data["data_completeness"]["quality_flags"] == ["fixture_invalid"]


def test_report_pdf_contains_a_valid_paginated_pdf_and_graph() -> None:
    """Synthetic empty report fixture exercises empty chart and PDF generation."""
    from app.models import Report
    owner = Owner(username="synthetic_pdf_owner", display_name="synthetic pdf owner")
    device = Device(device_identifier="synthetic-pdf-device")
    session = MeasurementSession(device_id=uuid4(), lifecycle="created", data_provenance="synthetic_test")
    report = Report(id=uuid4(), session_id=session.id, report_version="v1", generator_version="test")
    data = build_report_data(report, session, device, [], [], [], 0, datetime.now(timezone.utc))
    pdf = generate_pdf(data)
    assert pdf.startswith(b"%PDF-")
    assert len(pdf) > 1000


def test_report_endpoints_are_owner_scoped_and_idempotent(client: TestClient, db_session) -> None:
    owner, _device, session, _batch = make_session(db_session, completed=True)
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    def override_db():
        with factory() as request_db:
            yield request_db
    client.app.dependency_overrides[get_db_session] = override_db
    client.app.dependency_overrides[get_current_owner_username] = lambda: owner.username
    path = f"/api/v1/sessions/{session.id}/report"
    first = client.post(path)
    assert first.status_code == 201, first.text
    second = client.post(path)
    assert second.status_code == 200, second.text
    assert first.json()["id"] == second.json()["id"]
    assert client.get(path).json()["content_sha256"] == first.json()["content_sha256"]
    pdf_response = client.get(f"{path}.pdf")
    assert pdf_response.status_code == 200
    assert pdf_response.content.startswith(b"%PDF-")
    client.app.dependency_overrides[get_current_owner_username] = lambda: "another_owner"
    assert client.get(path).status_code == 404


def test_report_snapshot_does_not_interpolate_envelope(db_session) -> None:
    _owner, device, session, batch = make_session(db_session, completed=True)
    for seq, timestamp, value in [(0, 0, 0.1), (1, 4000, 0.4)]:
        db_session.add(TelemetrySample(sample_id=uuid4(), batch_row_id=batch.id, device_id=device.id,
            session_id=session.id, sample_sequence=seq, device_timestamp_ms=timestamp,
            acoustic_envelope=value, data_provenance="synthetic_test"))
    db_session.commit()
    report, _ = ReportService(db_session).get_or_create(session, device)
    assert len(report.report_data["acoustic_envelope"]["samples"]) == 2
    assert report.report_data["acoustic_envelope"]["interpolation"] is False


def test_pressure_is_calibration_gated_and_event_links_only_at_exact_device_time(db_session) -> None:
    _owner, device, session, batch = make_session(db_session, completed=True)
    device.pressure_calibration_confirmed = True
    device.pressure_calibration_version = "fixture-cal-v1"
    db_session.add(TelemetrySample(sample_id=uuid4(), batch_row_id=batch.id, device_id=device.id,
        session_id=session.id, sample_sequence=1, device_timestamp_ms=1000, pressure_mmhg=Decimal("80.0"),
        pressure_unit="mmHg", synchronization_metadata={"pressure_calibration_version": "fixture-cal-v1"},
        data_provenance="synthetic_test"))
    db_session.add(AcousticEvent(event_id=uuid4(), batch_row_id=batch.id, device_id=device.id,
        session_id=session.id, event_type="synthetic_event", device_timestamp_ms=1000, is_valid=True,
        data_provenance="synthetic_test"))
    db_session.add(AcousticEvent(event_id=uuid4(), batch_row_id=batch.id, device_id=device.id,
        session_id=session.id, event_type="different_timestamp", device_timestamp_ms=1001, is_valid=True,
        data_provenance="synthetic_test"))
    db_session.commit()
    report, _ = ReportService(db_session).get_or_create(session, device)
    assert report.report_data["pressure_trace"]["samples"][0]["mmHg"] == 80.0
    event_links = [event["pressure_association"] for event in report.report_data["acoustic_events"]]
    assert sum(link is not None for link in event_links) == 1
    assert next(link for link in event_links if link is not None)["method"] == "exact_device_timestamp"
