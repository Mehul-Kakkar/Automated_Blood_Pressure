"""Versioned MQTT envelopes. These are proposed future device contracts."""

from datetime import datetime
from enum import Enum
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from app.schemas.telemetry import TelemetryBatchInput


class StrictEnvelope(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)


class TelemetryEnvelope(StrictEnvelope):
    schema_version: Literal["1"]
    payload: TelemetryBatchInput


class DeviceReportedState(str, Enum):
    IDLE = "idle"
    MEASURING = "measuring"
    STOPPED = "stopped"
    FAULT = "fault"
    DATA_GAP = "data_gap"


class DeviceStatusMessage(StrictEnvelope):
    schema_version: Literal["1"]
    message_id: UUID
    device_id: UUID
    sequence: int = Field(ge=0, le=2**63 - 1)
    state: DeviceReportedState
    device_uptime_ms: int | None = Field(default=None, ge=0, le=2**63 - 1)
    firmware_version: str | None = Field(default=None, max_length=64)
    processing_algorithm_version: str | None = Field(default=None, max_length=64)
    session_id: UUID | None = None
    lost_sample_count: int = Field(default=0, ge=0, le=2**63 - 1)
    reported_at: datetime | None = None

    @field_validator("reported_at")
    @classmethod
    def reported_time_is_aware(cls, value: datetime | None) -> datetime | None:
        if value is not None and value.utcoffset() is None:
            raise ValueError("reported_at must include a timezone offset")
        return value


class DeviceCommandType(str, Enum):
    START = "START"
    STOP = "STOP"
    STATUS_REQUEST = "STATUS_REQUEST"


class DeviceCommandRequest(StrictEnvelope):
    schema_version: Literal["1"] = "1"
    command_id: UUID
    device_id: UUID
    session_id: UUID | None = None
    command_type: DeviceCommandType
    issued_at: datetime
    expires_at: datetime

    @field_validator("issued_at", "expires_at")
    @classmethod
    def command_times_are_aware(cls, value: datetime) -> datetime:
        if value.utcoffset() is None:
            raise ValueError("command timestamps must include a timezone offset")
        return value

    @model_validator(mode="after")
    def session_required_for_session_commands(self) -> "DeviceCommandRequest":
        if self.command_type in {DeviceCommandType.START, DeviceCommandType.STOP} and self.session_id is None:
            raise ValueError("START and STOP require a session_id")
        if self.expires_at <= self.issued_at:
            raise ValueError("expires_at must be later than issued_at")
        return self


class DeviceCommandResponse(StrictEnvelope):
    schema_version: Literal["1"]
    response_id: UUID
    command_id: UUID
    device_id: UUID
    status: Literal["accepted", "rejected", "completed"]
    reason_code: str | None = Field(default=None, pattern=r"^[A-Za-z0-9_.-]{1,64}$")
    device_state: DeviceReportedState | None = None
    reported_at: datetime | None = None

    @field_validator("reported_at")
    @classmethod
    def reported_time_is_aware(cls, value: datetime | None) -> datetime | None:
        if value is not None and value.utcoffset() is None:
            raise ValueError("reported_at must include a timezone offset")
        return value
