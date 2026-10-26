"""Strict future telemetry ingestion schemas; not an existing ESP32 contract."""

from datetime import datetime
from decimal import Decimal
from typing import Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

MAX_BATCH_ITEMS = 500
MAX_BATCH_BYTES = 262144


class StrictPayload(BaseModel):
    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)


class TelemetrySampleInput(StrictPayload):
    sample_id: UUID
    sample_sequence: int | None = Field(default=None, ge=0, le=2**63 - 1)
    device_timestamp_ms: int | None = Field(default=None, ge=0, le=2**63 - 1)
    device_recorded_at: datetime | None = None
    pressure_mmhg: Decimal | None = Field(
        default=None, ge=0, le=400, allow_inf_nan=False, max_digits=7, decimal_places=2
    )
    heart_rate_bpm: int | None = Field(default=None, ge=25, le=240)
    raw_adc: int | None = Field(default=None, ge=0, le=4095)
    filtered: Decimal | None = Field(default=None, allow_inf_nan=False, max_digits=12, decimal_places=4)
    acoustic_envelope: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False, max_digits=12, decimal_places=4)
    noise_floor: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False, max_digits=12, decimal_places=4)
    threshold: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False, max_digits=12, decimal_places=4)
    event_detected: bool | None = None
    is_valid: bool = True
    quality_flags: list[str] = Field(default_factory=list, max_length=16)

    @field_validator("device_recorded_at")
    @classmethod
    def timestamp_must_include_timezone(cls, value: datetime | None) -> datetime | None:
        if value is not None and value.utcoffset() is None:
            raise ValueError("device_recorded_at must include a timezone offset")
        return value

    @field_validator("quality_flags")
    @classmethod
    def quality_flags_must_be_bounded(cls, value: list[str]) -> list[str]:
        if any(not flag or len(flag) > 64 for flag in value):
            raise ValueError("quality flags must contain 1 to 64 characters")
        return value


class AcousticEventInput(StrictPayload):
    event_id: UUID
    event_sequence: int | None = Field(default=None, ge=0, le=2**63 - 1)
    event_type: str = Field(min_length=1, max_length=48)
    device_timestamp_ms: int | None = Field(default=None, ge=0, le=2**63 - 1)
    device_recorded_at: datetime | None = None
    strength: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False, max_digits=12, decimal_places=4)
    pressure_mmhg: Decimal | None = Field(
        default=None, ge=0, le=400, allow_inf_nan=False, max_digits=7, decimal_places=2
    )
    is_valid: bool = True
    quality_flags: list[str] = Field(default_factory=list, max_length=16)
    metadata: dict[str, str | int | float | bool | None] = Field(default_factory=dict, max_length=16)

    @field_validator("device_recorded_at")
    @classmethod
    def timestamp_must_include_timezone(cls, value: datetime | None) -> datetime | None:
        if value is not None and value.utcoffset() is None:
            raise ValueError("device_recorded_at must include a timezone offset")
        return value

    @field_validator("quality_flags")
    @classmethod
    def quality_flags_must_be_bounded(cls, value: list[str]) -> list[str]:
        if any(not flag or len(flag) > 64 for flag in value):
            raise ValueError("quality flags must contain 1 to 64 characters")
        return value

    @field_validator("metadata")
    @classmethod
    def metadata_must_be_small_and_finite(
        cls, value: dict[str, str | int | float | bool | None]
    ) -> dict[str, str | int | float | bool | None]:
        import math

        for key, item in value.items():
            if not key or len(key) > 64:
                raise ValueError("metadata keys must contain 1 to 64 characters")
            if isinstance(item, float) and not math.isfinite(item):
                raise ValueError("metadata numeric values must be finite")
            if isinstance(item, str) and len(item) > 256:
                raise ValueError("metadata text values cannot exceed 256 characters")
        return value


class TelemetryBatchInput(StrictPayload):
    batch_id: UUID
    device_id: UUID
    session_id: UUID
    schema_version: str = Field(min_length=1, max_length=32)
    firmware_version: str | None = Field(default=None, max_length=64)
    processing_algorithm_version: str | None = Field(default=None, max_length=64)
    data_provenance: Literal["device_live", "device_reconciled"]
    samples: list[TelemetrySampleInput] = Field(min_length=1, max_length=MAX_BATCH_ITEMS)
    acoustic_events: list[AcousticEventInput] = Field(default_factory=list, max_length=MAX_BATCH_ITEMS)

    @model_validator(mode="after")
    def identifiers_are_unique_within_batch(self) -> "TelemetryBatchInput":
        if len(self.samples) + len(self.acoustic_events) > MAX_BATCH_ITEMS:
            raise ValueError(f"a batch cannot contain more than {MAX_BATCH_ITEMS} total records")
        sample_ids = [sample.sample_id for sample in self.samples]
        event_ids = [event.event_id for event in self.acoustic_events]
        sample_sequences = [sample.sample_sequence for sample in self.samples if sample.sample_sequence is not None]
        event_sequences = [event.event_sequence for event in self.acoustic_events if event.event_sequence is not None]
        if len(sample_ids) != len(set(sample_ids)) or len(event_ids) != len(set(event_ids)):
            raise ValueError("record identifiers must be unique within a batch")
        if len(sample_sequences) != len(set(sample_sequences)) or len(event_sequences) != len(set(event_sequences)):
            raise ValueError("record sequence values must be unique within a batch")
        return self

    def canonical_payload_bytes(self) -> bytes:
        import json

        return json.dumps(self.model_dump(mode="json"), sort_keys=True, separators=(",", ":")).encode("utf-8")

    def validate_size(self, max_bytes: int = MAX_BATCH_BYTES) -> bytes:
        payload = self.canonical_payload_bytes()
        if len(payload) > max_bytes:
            raise ValueError(f"batch payload exceeds the {max_bytes}-byte limit")
        return payload
