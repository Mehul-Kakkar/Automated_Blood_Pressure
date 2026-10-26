"""Owner-scoped session query and state transition helpers."""

from datetime import datetime, timedelta, timezone
from uuid import UUID, uuid4

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.core.config import Settings
from app.models import AcousticEvent, Device, DeviceCommand, MeasurementSession, SessionEvent, TelemetrySample
from app.messaging.schemas import DeviceCommandType
from app.schemas.sessions import CommandSummary, SessionResponse


def utc(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def owned_session(db: Session, owner_id: UUID, session_id: UUID) -> tuple[MeasurementSession, Device] | None:
    row = db.execute(
        select(MeasurementSession, Device)
        .join(Device, Device.id == MeasurementSession.device_id)
        .where(MeasurementSession.id == session_id, Device.owner_id == owner_id)
    ).first()
    return (row[0], row[1]) if row else None


def build_session_response(db: Session, session: MeasurementSession, device: Device, settings: Settings) -> SessionResponse:
    latest = db.scalar(
        select(DeviceCommand)
        .where(DeviceCommand.session_id == session.id)
        .order_by(DeviceCommand.issued_at.desc(), DeviceCommand.id.desc())
        .limit(1)
    )
    sample_count = db.scalar(select(func.count()).select_from(TelemetrySample).where(TelemetrySample.session_id == session.id)) or 0
    event_count = db.scalar(select(func.count()).select_from(AcousticEvent).where(AcousticEvent.session_id == session.id)) or 0
    seen = utc(device.state_reported_at)
    fresh = seen is not None and seen >= datetime.now(timezone.utc) - timedelta(seconds=settings.device_state_max_age_seconds)
    return SessionResponse(
        id=session.id,
        device_id=session.device_id,
        lifecycle=session.lifecycle,
        created_at=session.created_at,
        updated_at=session.updated_at,
        started_at=session.started_at,
        ended_at=session.ended_at,
        termination_reason=session.termination_reason,
        reported_device_state=device.reported_state if fresh else None,
        device_last_seen_at=device.last_seen_at,
        device_state_fresh=fresh,
        latest_command=CommandSummary.model_validate(latest, from_attributes=True) if latest else None,
        sample_count=sample_count,
        acoustic_event_count=event_count,
        data_provenance=session.data_provenance,
    )


def record_session_event(
    db: Session,
    *,
    device_id: UUID,
    session_id: UUID | None,
    event_type: str,
    data: dict[str, object],
    event_id: UUID | None = None,
    payload_sha256: str | None = None,
    device_sequence: int | None = None,
    device_recorded_at: datetime | None = None,
) -> None:
    db.add(
        SessionEvent(
            event_id=event_id or uuid4(),
            device_id=device_id,
            session_id=session_id,
            event_type=event_type,
            payload_sha256=payload_sha256,
            data=data,
            device_sequence=device_sequence,
            device_recorded_at=device_recorded_at,
        )
    )


def make_command(device_id: UUID, session_id: UUID, command_type: DeviceCommandType, ttl_seconds: int):
    from app.messaging.schemas import DeviceCommandRequest

    issued = datetime.now(timezone.utc)
    return DeviceCommandRequest(
        command_id=uuid4(),
        device_id=device_id,
        session_id=session_id,
        command_type=command_type,
        issued_at=issued,
        expires_at=issued + timedelta(seconds=ttl_seconds),
    )
