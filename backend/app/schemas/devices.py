"""Owner-scoped device registration and status schemas."""
from datetime import datetime
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field

class DeviceRegistrationRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)
    device_identifier: str = Field(min_length=1, max_length=128)
    hardware_revision: str | None = Field(default=None, max_length=64)

class DeviceResponse(BaseModel):
    id: UUID
    device_identifier: str
    hardware_revision: str | None
    firmware_version: str | None
    processing_algorithm_version: str | None
    mqtt_enrolled: bool
    online: bool
    reported_state: str | None
    state_reported_at: datetime | None
    last_seen_at: datetime | None
    pressure_calibration_confirmed: bool
    pressure_calibration_version: str | None

class DeviceListResponse(BaseModel):
    items: list[DeviceResponse]
