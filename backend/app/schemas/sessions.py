"""Typed owner-scoped session, telemetry, and event API representations."""

from datetime import datetime
from decimal import Decimal
from typing import Any
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field


class SessionCreateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    device_id: UUID
    idempotency_key: UUID


class CommandSummary(BaseModel):
    command_id: UUID
    command_type: str
    status: str
    issued_at: datetime
    expires_at: datetime
    response_code: str | None


class SessionResponse(BaseModel):
    id: UUID
    device_id: UUID
    lifecycle: str
    created_at: datetime
    updated_at: datetime
    started_at: datetime | None
    ended_at: datetime | None
    termination_reason: str | None
    reported_device_state: str | None
    device_last_seen_at: datetime | None
    device_state_fresh: bool
    latest_command: CommandSummary | None
    sample_count: int
    acoustic_event_count: int
    data_provenance: str


class SessionPage(BaseModel):
    items: list[SessionResponse]
    limit: int
    offset: int
    total: int


class RawAcousticSample(BaseModel):
    adc_counts: int | None


class ProcessedSignalSample(BaseModel):
    filtered: Decimal | None
    envelope: Decimal | None
    noise_floor: Decimal | None
    threshold: Decimal | None


class TelemetrySampleResponse(BaseModel):
    sample_id: UUID
    sequence: int | None
    device_timestamp_ms: int | None
    device_recorded_at: datetime | None
    server_received_at: datetime
    raw_acoustic_sample: RawAcousticSample
    processed_signal: ProcessedSignalSample
    event_detected: bool | None
    cuff_pressure_mmhg: Decimal | None
    pressure_calibration_version: str | None
    heart_rate_bpm: int | None
    is_valid: bool
    quality_flags: list[str]
    firmware_version: str | None
    processing_algorithm_version: str | None
    data_provenance: str


class AcousticEventResponse(BaseModel):
    event_id: UUID
    event_sequence: int | None
    event_type: str
    device_timestamp_ms: int | None
    device_recorded_at: datetime | None
    server_received_at: datetime
    strength: Decimal | None
    is_valid: bool
    quality_flags: list[str]
    algorithm_version: str | None
    metadata: dict[str, Any]
    associated_pressure_sample_id: UUID | None = None


class SessionSamplesPage(BaseModel):
    samples: list[TelemetrySampleResponse]
    acoustic_events: list[AcousticEventResponse]
    limit: int
    offset: int
    total_samples: int
    total_acoustic_events: int
    next_offset: int | None
    derived_measurements_available: bool = False


class SessionEventResponse(BaseModel):
    id: int
    event_id: UUID
    session_id: UUID | None
    event_type: str
    device_sequence: int | None
    device_recorded_at: datetime | None
    server_received_at: datetime
    data: dict[str, Any]


class SessionEventsPage(BaseModel):
    items: list[SessionEventResponse]
    after_id: int
    limit: int
    next_after_id: int | None


class PaginationParams(BaseModel):
    limit: int = Field(default=50, ge=1, le=200)
    offset: int = Field(default=0, ge=0, le=1_000_000)
