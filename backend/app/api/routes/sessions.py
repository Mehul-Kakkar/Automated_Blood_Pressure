"""Owner-authorized measurement session and telemetry endpoints."""

import asyncio
import json
import logging
from collections.abc import AsyncIterator
from datetime import datetime, timedelta, timezone
from typing import Annotated
from uuid import UUID

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.responses import StreamingResponse
from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.core.config import Settings
from app.db.session import get_db_session, get_session_factory
from app.messaging.schemas import DeviceCommandType
from app.messaging.service import BrokerUnavailable, CommandNotAuthorized, MessagingError, MessagingService
from app.models import AcousticEvent, Device, MeasurementSession, SessionEvent, TelemetrySample
from app.schemas.sessions import (
    AcousticEventResponse,
    ProcessedSignalSample,
    RawAcousticSample,
    SessionCreateRequest,
    SessionEventResponse,
    SessionEventsPage,
    SessionPage,
    SessionResponse,
    SessionSamplesPage,
    TelemetrySampleResponse,
)
from app.security.auth import get_current_owner_username
from app.services.session_lifecycle import build_session_response, make_command, owned_session, record_session_event, utc

logger = logging.getLogger("api.sessions")
router = APIRouter(prefix="/api/v1/sessions", tags=["sessions"])
Db = Annotated[Session, Depends(get_db_session)]
OwnerUsername = Annotated[str, Depends(get_current_owner_username)]


def _require_owned_device(db: Session, owner_username: str, device_id: UUID) -> Device:
    device = db.scalar(select(Device).where(Device.id == device_id, Device.owner_username == owner_username))
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    return device


def _require_messaging(request: Request) -> MessagingService:
    service = getattr(request.app.state, "messaging", None)
    if service is None:
        raise HTTPException(status_code=503, detail="Device messaging is unavailable.")
    return service


def _require_fresh_state(device: Device, settings: Settings, expected: str) -> None:
    seen = utc(device.state_reported_at)
    fresh = seen is not None and seen >= datetime.now(timezone.utc) - timedelta(
        seconds=settings.device_state_max_age_seconds
    )
    if not fresh or device.reported_state != expected:
        raise HTTPException(status_code=409, detail="The device state is stale or does not allow this transition.")


def _view(db: Session, session: MeasurementSession, device: Device, settings: Settings) -> SessionResponse:
    return build_session_response(db, session, device, settings)


@router.post("", response_model=SessionResponse, status_code=status.HTTP_202_ACCEPTED)
def create_session(
    body: SessionCreateRequest,
    request: Request,
    response: Response,
    db: Db,
    owner_username: OwnerUsername,
) -> SessionResponse:
    settings: Settings = request.app.state.settings
    device = _require_owned_device(db, owner_username, body.device_id)
    existing = db.scalar(
        select(MeasurementSession).where(
            MeasurementSession.device_id == device.id,
            MeasurementSession.client_request_id == body.idempotency_key,
        )
    )
    if existing is not None:
        response.status_code = status.HTTP_200_OK
        return _view(db, existing, device, settings)
    if not device.mqtt_enabled or device.mqtt_principal != str(device.id):
        raise HTTPException(status_code=409, detail="The device is not enrolled for remote messaging.")
    _require_fresh_state(device, settings, "idle")

    session = MeasurementSession(
        device_id=device.id,
        client_request_id=body.idempotency_key,
        lifecycle="created",
        data_provenance="device_live",
    )
    db.add(session)
    try:
        db.flush()
    except IntegrityError:
        db.rollback()
        duplicate_session = db.scalar(
            select(MeasurementSession).where(
                MeasurementSession.device_id == device.id,
                MeasurementSession.client_request_id == body.idempotency_key,
            )
        )
        if duplicate_session is None:
            raise HTTPException(status_code=409, detail="The session request conflicts with existing data.")
        response.status_code = status.HTTP_200_OK
        return _view(db, duplicate_session, device, settings)
    record_session_event(
        db,
        device_id=device.id,
        session_id=session.id,
        event_type="session_created",
        data={"lifecycle": "created"},
    )
    db.commit()
    command = None
    try:
        service = _require_messaging(request)
        command = make_command(device.id, session.id, DeviceCommandType.START, settings.mqtt_command_ttl_seconds)
        service.publish_command(command, owner_username=owner_username)
    except MessagingError as exc:
        db.refresh(session)
        if session.lifecycle == "created":
            session.lifecycle = "error"
            session.termination_reason = "start_command_failed"
            record_session_event(
                db,
                device_id=device.id,
                session_id=session.id,
                event_type="command_failure",
                event_id=command.command_id if command is not None else None,
                data={"command_type": "START", "failure": "not_sent_or_unconfirmed"},
            )
            db.commit()
        logger.info("session_start_command_failed", extra={"event": "session_start_command_failed"})
        status_code = 403 if isinstance(exc, CommandNotAuthorized) else 503
        raise HTTPException(status_code=status_code, detail="The device did not confirm a start request.") from exc
    db.refresh(session)
    return _view(db, session, device, settings)


@router.get("", response_model=SessionPage)
def list_sessions(
    request: Request,
    db: Db,
    owner_username: OwnerUsername,
    limit: Annotated[int, Query(ge=1, le=100)] = 50,
    offset: Annotated[int, Query(ge=0, le=1_000_000)] = 0,
) -> SessionPage:
    filters = Device.owner_username == owner_username
    total = db.scalar(
        select(func.count()).select_from(MeasurementSession).join(Device).where(filters)
    ) or 0
    rows = db.execute(
        select(MeasurementSession, Device)
        .join(Device, Device.id == MeasurementSession.device_id)
        .where(filters)
        .order_by(MeasurementSession.created_at.desc(), MeasurementSession.id.desc())
        .limit(limit)
        .offset(offset)
    ).all()
    settings: Settings = request.app.state.settings
    return SessionPage(items=[_view(db, session, device, settings) for session, device in rows], limit=limit, offset=offset, total=total)


@router.get("/{session_id}", response_model=SessionResponse)
def get_session(session_id: UUID, request: Request, db: Db, owner_username: OwnerUsername) -> SessionResponse:
    result = owned_session(db, owner_username, session_id)
    if result is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    return _view(db, result[0], result[1], request.app.state.settings)


@router.post("/{session_id}/complete", response_model=SessionResponse, status_code=status.HTTP_202_ACCEPTED)
def request_session_completion(
    session_id: UUID, request: Request, response: Response, db: Db, owner_username: OwnerUsername
) -> SessionResponse:
    result = owned_session(db, owner_username, session_id)
    if result is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    session, device = result
    if session.lifecycle == "completed":
        response.status_code = status.HTTP_200_OK
        return _view(db, session, device, request.app.state.settings)
    if session.lifecycle == "stopping":
        return _view(db, session, device, request.app.state.settings)
    if session.lifecycle not in {"measuring", "interrupted"}:
        raise HTTPException(status_code=409, detail="Only a device-confirmed measuring session can be stopped.")
    _require_fresh_state(device, request.app.state.settings, "measuring")
    service = _require_messaging(request)
    try:
        service.publish_command(
            make_command(device.id, session.id, DeviceCommandType.STOP, request.app.state.settings.mqtt_command_ttl_seconds),
            owner_username=owner_username,
        )
    except MessagingError as exc:
        logger.info("session_stop_command_failed", extra={"event": "session_stop_command_failed"})
        raise HTTPException(status_code=503, detail="The device has not confirmed a stop request.") from exc
    db.refresh(session)
    return _view(db, session, device, request.app.state.settings)


@router.post("/{session_id}/cancel", response_model=SessionResponse)
def cancel_session(session_id: UUID, request: Request, db: Db, owner_username: OwnerUsername) -> SessionResponse:
    result = owned_session(db, owner_username, session_id)
    if result is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    session, device = result
    if session.lifecycle == "cancelled":
        return _view(db, session, device, request.app.state.settings)
    if session.lifecycle == "interrupted" and session.started_at is None:
        _require_fresh_state(device, request.app.state.settings, device.reported_state or "")
        if device.reported_state not in {"idle", "stopped"}:
            raise HTTPException(status_code=409, detail="The device has not confirmed an idle state for cancellation.")
        session.termination_reason = "cancelled_after_idle_confirmation"
    elif session.lifecycle == "created":
        session.termination_reason = "cancelled_before_start"
    else:
        raise HTTPException(status_code=409, detail="A started or start-requested session cannot be cancelled locally.")
    session.lifecycle = "cancelled"
    session.ended_at = datetime.now(timezone.utc)
    record_session_event(
        db, device_id=device.id, session_id=session.id, event_type="session_cancelled",
        data={"reason": session.termination_reason},
    )
    db.commit()
    return _view(db, session, device, request.app.state.settings)


@router.get("/{session_id}/samples", response_model=SessionSamplesPage)
def get_session_samples(
    session_id: UUID,
    db: Db,
    owner_username: OwnerUsername,
    limit: Annotated[int, Query(ge=1, le=200)] = 100,
    offset: Annotated[int, Query(ge=0, le=1_000_000)] = 0,
) -> SessionSamplesPage:
    if owned_session(db, owner_username, session_id) is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    sample_count = db.scalar(select(func.count()).select_from(TelemetrySample).where(TelemetrySample.session_id == session_id)) or 0
    event_count = db.scalar(select(func.count()).select_from(AcousticEvent).where(AcousticEvent.session_id == session_id)) or 0
    rows = db.scalars(
        select(TelemetrySample).where(TelemetrySample.session_id == session_id)
        .order_by(TelemetrySample.received_at, TelemetrySample.id).limit(limit).offset(offset)
    ).all()
    acoustic_rows = db.scalars(
        select(AcousticEvent).where(AcousticEvent.session_id == session_id)
        .order_by(AcousticEvent.received_at, AcousticEvent.id).limit(limit).offset(offset)
    ).all()
    samples = [TelemetrySampleResponse(
        sample_id=row.sample_id, sequence=row.sample_sequence, device_timestamp_ms=row.device_timestamp_ms,
        device_recorded_at=row.device_recorded_at, server_received_at=row.received_at,
        raw_acoustic_sample=RawAcousticSample(adc_counts=row.raw_adc),
        processed_signal=ProcessedSignalSample(filtered=row.filtered, envelope=row.acoustic_envelope,
            noise_floor=row.noise_floor, threshold=row.threshold),
        event_detected=row.event_detected, cuff_pressure_mmhg=row.pressure_mmhg,
        pressure_calibration_version=row.synchronization_metadata.get("pressure_calibration_version"),
        heart_rate_bpm=row.heart_rate_bpm, is_valid=row.is_valid, quality_flags=row.quality_flags,
        firmware_version=row.firmware_version, processing_algorithm_version=row.processing_algorithm_version,
        data_provenance=row.data_provenance,
    ) for row in rows]
    acoustic_events = [AcousticEventResponse(
        event_id=row.event_id, event_sequence=row.event_sequence, event_type=row.event_type,
        device_timestamp_ms=row.device_timestamp_ms, device_recorded_at=row.device_recorded_at,
        server_received_at=row.received_at, strength=row.strength, is_valid=row.is_valid,
        quality_flags=row.quality_flags, algorithm_version=row.processing_algorithm_version,
        metadata=row.metadata_json, associated_pressure_sample_id=None,
    ) for row in acoustic_rows]
    next_offset = offset + limit if offset + limit < max(sample_count, event_count) else None
    return SessionSamplesPage(
        samples=samples, acoustic_events=acoustic_events, limit=limit, offset=offset,
        total_samples=sample_count, total_acoustic_events=event_count, next_offset=next_offset,
    )


def _event_response(event: SessionEvent) -> SessionEventResponse:
    return SessionEventResponse(
        id=event.id, event_id=event.event_id, session_id=event.session_id, event_type=event.event_type,
        device_sequence=event.device_sequence, device_recorded_at=event.device_recorded_at,
        server_received_at=event.received_at, data=event.data,
    )


@router.get("/{session_id}/events", response_model=SessionEventsPage)
def get_session_events(
    session_id: UUID, db: Db, owner_username: OwnerUsername,
    after_id: Annotated[int, Query(ge=0)] = 0,
    limit: Annotated[int, Query(ge=1, le=200)] = 100,
) -> SessionEventsPage:
    if owned_session(db, owner_username, session_id) is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    rows = db.scalars(
        select(SessionEvent).where(SessionEvent.session_id == session_id, SessionEvent.id > after_id)
        .order_by(SessionEvent.id).limit(limit + 1)
    ).all()
    rows = rows[:limit]
    return SessionEventsPage(
        items=[_event_response(row) for row in rows], after_id=after_id, limit=limit,
        next_after_id=rows[-1].id if rows else after_id,
    )


@router.get("/{session_id}/events/stream")
async def stream_session_events(
    session_id: UUID,
    request: Request,
    db: Db,
    owner_username: OwnerUsername,
    last_event_id: Annotated[int | None, Header(alias="Last-Event-ID")] = None,
    after_id: Annotated[int, Query(ge=0)] = 0,
    max_events: Annotated[int | None, Query(ge=1, le=1000)] = None,
) -> StreamingResponse:
    if owned_session(db, owner_username, session_id) is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    cursor = max(after_id, last_event_id or 0)

    async def generate() -> AsyncIterator[str]:
        nonlocal cursor
        factory = get_session_factory()
        heartbeat = 0
        sent_events = 0
        first_iteration = True
        while first_iteration or not await request.is_disconnected():
            first_iteration = False
            with factory() as stream_db:
                rows = stream_db.scalars(
                    select(SessionEvent).where(SessionEvent.session_id == session_id, SessionEvent.id > cursor)
                    .order_by(SessionEvent.id).limit(100)
                ).all()
                rendered = []
                for row in rows:
                    payload = _event_response(row).model_dump(mode="json")
                    if row.event_type == "telemetry_batch":
                        payload["samples_url"] = f"/api/v1/sessions/{session_id}/samples"
                    rendered.append(f"id: {row.id}\nevent: {row.event_type}\ndata: {json.dumps(payload, separators=(',', ':'))}\n\n")
                    cursor = row.id
            if rendered:
                for item in rendered:
                    yield item
                    sent_events += 1
                    if max_events is not None and sent_events >= max_events:
                        return
                heartbeat = 0
            else:
                heartbeat += 1
                if heartbeat >= 15:
                    yield ": keep-alive\n\n"
                    heartbeat = 0
                await asyncio.sleep(1)

    return StreamingResponse(
        generate(), media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )
