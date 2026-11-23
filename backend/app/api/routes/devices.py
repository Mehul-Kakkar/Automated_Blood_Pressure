"""Authenticated device registration and status endpoints."""
from datetime import datetime, timedelta, timezone
from typing import Annotated
from uuid import UUID
from fastapi import APIRouter, Depends, HTTPException, Request, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session
from app.db.session import get_db_session
from app.messaging.schemas import DeviceCommandType
from app.messaging.service import MessagingError, MessagingService
from app.models import Device, DeviceCommand
from app.schemas.devices import DeviceCommandResponse, DeviceListResponse, DeviceRegistrationRequest, DeviceResponse, MqttEnrollmentRequest
from app.services.session_lifecycle import make_command
from app.security.auth import get_current_owner_username

router = APIRouter(prefix="/api/v1/devices", tags=["devices"])
Db = Annotated[Session, Depends(get_db_session)]
OwnerUsername = Annotated[str, Depends(get_current_owner_username)]

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
def register_device(body: DeviceRegistrationRequest, request: Request, db: Db, owner_username: OwnerUsername) -> DeviceResponse:
    device = Device(owner_username=owner_username, device_identifier=body.device_identifier,
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
def list_devices(request: Request, db: Db, owner_username: OwnerUsername) -> DeviceListResponse:
    devices = db.scalars(select(Device).where(Device.owner_username == owner_username).order_by(Device.registered_at.desc())).all()
    max_age = request.app.state.settings.device_state_max_age_seconds
    return DeviceListResponse(items=[_view(device, max_age) for device in devices])

@router.get("/{device_id}", response_model=DeviceResponse)
def get_device(device_id: UUID, request: Request, db: Db, owner_username: OwnerUsername) -> DeviceResponse:
    device = db.scalar(select(Device).where(Device.id == device_id, Device.owner_username == owner_username))
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    return _view(device, request.app.state.settings.device_state_max_age_seconds)


@router.post("/{device_id}/mqtt-enrollment", response_model=DeviceResponse)
def enroll_mqtt_device(
    device_id: UUID, body: MqttEnrollmentRequest, request: Request, db: Db, owner_username: OwnerUsername
) -> DeviceResponse:
    device = db.scalar(
        select(Device).where(Device.id == device_id, Device.owner_username == owner_username).with_for_update()
    )
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    if device.mqtt_enabled:
        if device.mqtt_principal != body.mqtt_principal:
            raise HTTPException(status_code=409, detail="The device is already enrolled with a different MQTT principal.")
        return _view(device, request.app.state.settings.device_state_max_age_seconds)
    device.mqtt_principal = body.mqtt_principal
    device.mqtt_enabled = True
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise HTTPException(status_code=409, detail="That MQTT principal is already associated with another device.") from exc
    db.refresh(device)
    return _view(device, request.app.state.settings.device_state_max_age_seconds)


@router.post("/{device_id}/status-request", response_model=DeviceCommandResponse, status_code=status.HTTP_202_ACCEPTED)
def request_device_status(
    device_id: UUID, request: Request, db: Db, owner_username: OwnerUsername
) -> DeviceCommandResponse:
    device = db.scalar(select(Device).where(Device.id == device_id, Device.owner_username == owner_username))
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    if not device.mqtt_enabled or not device.mqtt_principal:
        raise HTTPException(status_code=409, detail="Enroll the registered device for MQTT before requesting status.")
    service: MessagingService | None = getattr(request.app.state, "messaging", None)
    if service is None:
        raise HTTPException(status_code=503, detail="Device messaging is unavailable.")
    command = make_command(device.id, None, DeviceCommandType.STATUS_REQUEST, request.app.state.settings.mqtt_command_ttl_seconds)
    try:
        row = service.publish_command(command, owner_username=owner_username)
    except MessagingError as exc:
        raise HTTPException(status_code=503, detail="The status request could not be sent to the device.") from exc
    return DeviceCommandResponse.model_validate(row, from_attributes=True)


@router.get("/{device_id}/commands/{command_id}", response_model=DeviceCommandResponse)
def get_device_command(
    device_id: UUID, command_id: UUID, db: Db, owner_username: OwnerUsername
) -> DeviceCommandResponse:
    device = db.scalar(select(Device).where(Device.id == device_id, Device.owner_username == owner_username))
    if device is None:
        raise HTTPException(status_code=404, detail="Device was not found.")
    command = db.scalar(
        select(DeviceCommand).where(DeviceCommand.command_id == command_id, DeviceCommand.device_id == device.id)
    )
    if command is None:
        raise HTTPException(status_code=404, detail="Device command was not found.")
    return DeviceCommandResponse.model_validate(command, from_attributes=True)
