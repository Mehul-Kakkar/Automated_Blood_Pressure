"""Authenticated device registration and status endpoints."""
from datetime import datetime, timedelta, timezone
from typing import Annotated
from uuid import UUID
from fastapi import APIRouter, Depends, HTTPException, Request, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session
from app.db.session import get_db_session
from app.models import Device
from app.schemas.devices import DeviceListResponse, DeviceRegistrationRequest, DeviceResponse
from app.security.auth import get_current_owner_id

router = APIRouter(prefix="/api/v1/devices", tags=["devices"])
Db = Annotated[Session, Depends(get_db_session)]
OwnerId = Annotated[UUID, Depends(get_current_owner_id)]

def _view(device: Device, max_age: int) -> DeviceResponse:
    seen = device.state_reported_at
    if seen is not None and seen.tzinfo is None:
        seen = seen.replace(tzinfo=timezone.utc)
    online = bool(device.mqtt_enabled and seen is not None and seen >= datetime.now(timezone.utc) - timedelta(seconds=max_age))
    return DeviceResponse(id=device.id, device_identifier=device.device_identifier,
        hardware_revision=device.hardware_revision, firmware_version=device.firmware_version,
        processing_algorithm_version=device.processing_algorithm_version, mqtt_enrolled=device.mqtt_enabled,
        online=online, reported_state=device.reported_state if online else None,
        state_reported_at=device.state_reported_at, last_seen_at=device.last_seen_at,
        pressure_calibration_confirmed=device.pressure_calibration_confirmed,
        pressure_calibration_version=device.pressure_calibration_version)

@router.post("", response_model=DeviceResponse, status_code=status.HTTP_201_CREATED)
def register_device(body: DeviceRegistrationRequest, request: Request, db: Db, owner_id: OwnerId) -> DeviceResponse:
    device = Device(owner_id=owner_id, device_identifier=body.device_identifier,
                    hardware_revision=body.hardware_revision, mqtt_enabled=False)
    db.add(device)
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise HTTPException(status_code=409, detail="Device identifier is already registered.") from exc
    db.refresh(device)
    return _view(device, request.app.state.settings.device_state_max_age_seconds)

@router.get("", response_model=DeviceListResponse)
def list_devices(request: Request, db: Db, owner_id: OwnerId) -> DeviceListResponse:
    devices = db.scalars(select(Device).where(Device.owner_id == owner_id).order_by(Device.registered_at.desc())).all()
    max_age = request.app.state.settings.device_state_max_age_seconds
    return DeviceListResponse(items=[_view(device, max_age) for device in devices])

@router.get("/{device_id}", response_model=DeviceResponse)
def get_device(device_id: UUID, request: Request, db: Db, owner_id: OwnerId) -> DeviceResponse:
    device = db.scalar(select(Device).where(Device.id == device_id, Device.owner_id == owner_id))
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    return _view(device, request.app.state.settings.device_state_max_age_seconds)
