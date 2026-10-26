"""Bounded telemetry batch, sample, and acoustic event records."""

from datetime import datetime
from decimal import Decimal
from uuid import UUID, uuid4

from sqlalchemy import (
    Boolean,
    BigInteger,
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    JSON,
    Numeric,
    String,
    UniqueConstraint,
    func,
)
from sqlalchemy.orm import Mapped, mapped_column, relationship
from sqlalchemy.types import Uuid

from app.db.base import Base


class TelemetryBatch(Base):
    __tablename__ = "telemetry_batches"
    __table_args__ = (
        UniqueConstraint("device_id", "batch_id", name="uq_telemetry_batches_device_id_batch_id"),
        CheckConstraint("sample_count BETWEEN 1 AND 500", name="sample_count_bounded"),
        CheckConstraint("event_count BETWEEN 0 AND 500", name="event_count_bounded"),
        Index("ix_telemetry_batches_session_received", "session_id", "received_at"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    batch_id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), nullable=False)
    device_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("devices.id", ondelete="RESTRICT"), nullable=False
    )
    session_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("measurement_sessions.id", ondelete="CASCADE"), nullable=False
    )
    payload_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    sample_count: Mapped[int] = mapped_column(Integer, nullable=False)
    event_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    schema_version: Mapped[str] = mapped_column(String(32), nullable=False)
    firmware_version: Mapped[str | None] = mapped_column(String(64))
    processing_algorithm_version: Mapped[str | None] = mapped_column(String(64))
    data_provenance: Mapped[str] = mapped_column(String(32), nullable=False)
    sync_status: Mapped[str] = mapped_column(String(24), nullable=False, default="received")
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)

    session: Mapped["MeasurementSession"] = relationship(back_populates="batches")


class TelemetrySample(Base):
    __tablename__ = "telemetry_samples"
    __table_args__ = (
        UniqueConstraint("device_id", "sample_id", name="uq_telemetry_samples_device_id_sample_id"),
        UniqueConstraint("session_id", "sample_sequence", name="uq_telemetry_samples_session_id_sample_sequence"),
        CheckConstraint("sample_sequence IS NULL OR sample_sequence >= 0", name="sample_sequence_nonnegative"),
        CheckConstraint("device_timestamp_ms IS NULL OR device_timestamp_ms >= 0", name="device_time_nonnegative"),
        CheckConstraint("pressure_mmhg IS NULL OR pressure_mmhg BETWEEN 0 AND 400", name="pressure_range"),
        CheckConstraint(
            "(pressure_mmhg IS NULL AND pressure_unit IS NULL) OR "
            "(pressure_mmhg IS NOT NULL AND pressure_unit IS NOT NULL AND pressure_unit = 'mmHg')",
            name="pressure_unit_explicit",
        ),
        CheckConstraint("heart_rate_bpm IS NULL OR heart_rate_bpm BETWEEN 25 AND 240", name="heart_rate_range"),
        CheckConstraint("raw_adc IS NULL OR raw_adc BETWEEN 0 AND 4095", name="raw_adc_range"),
        CheckConstraint("acoustic_envelope IS NULL OR acoustic_envelope >= 0", name="envelope_nonnegative"),
        CheckConstraint("noise_floor IS NULL OR noise_floor >= 0", name="noise_floor_nonnegative"),
        CheckConstraint("threshold IS NULL OR threshold >= 0", name="threshold_nonnegative"),
        Index("ix_telemetry_samples_session_received", "session_id", "received_at"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    sample_id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), nullable=False)
    batch_row_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("telemetry_batches.id", ondelete="CASCADE"), nullable=False
    )
    device_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("devices.id", ondelete="RESTRICT"), nullable=False
    )
    session_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("measurement_sessions.id", ondelete="CASCADE"), nullable=False
    )
    sample_sequence: Mapped[int | None] = mapped_column(BigInteger)
    device_timestamp_ms: Mapped[int | None] = mapped_column(BigInteger)
    device_recorded_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    pressure_mmhg: Mapped[Decimal | None] = mapped_column(Numeric(7, 2))
    pressure_unit: Mapped[str | None] = mapped_column(String(8))
    heart_rate_bpm: Mapped[int | None] = mapped_column(Integer)
    raw_adc: Mapped[int | None] = mapped_column(Integer)
    filtered: Mapped[Decimal | None] = mapped_column(Numeric(12, 4))
    acoustic_envelope: Mapped[Decimal | None] = mapped_column(Numeric(12, 4))
    noise_floor: Mapped[Decimal | None] = mapped_column(Numeric(12, 4))
    threshold: Mapped[Decimal | None] = mapped_column(Numeric(12, 4))
    event_detected: Mapped[bool | None] = mapped_column(Boolean)
    is_valid: Mapped[bool] = mapped_column(Boolean, nullable=False, default=True)
    quality_flags: Mapped[list[str]] = mapped_column(JSON, nullable=False, default=list)
    firmware_version: Mapped[str | None] = mapped_column(String(64))
    processing_algorithm_version: Mapped[str | None] = mapped_column(String(64))
    data_provenance: Mapped[str] = mapped_column(String(32), nullable=False)
    synchronization_metadata: Mapped[dict[str, object]] = mapped_column(JSON, nullable=False, default=dict)

    session: Mapped["MeasurementSession"] = relationship(back_populates="samples")


class AcousticEvent(Base):
    __tablename__ = "acoustic_events"
    __table_args__ = (
        UniqueConstraint("device_id", "event_id", name="uq_acoustic_events_device_id_event_id"),
        UniqueConstraint("session_id", "event_sequence", name="uq_acoustic_events_session_id_event_sequence"),
        CheckConstraint("event_sequence IS NULL OR event_sequence >= 0", name="event_sequence_nonnegative"),
        CheckConstraint("device_timestamp_ms IS NULL OR device_timestamp_ms >= 0", name="device_time_nonnegative"),
        CheckConstraint("strength IS NULL OR strength >= 0", name="strength_nonnegative"),
        CheckConstraint("pressure_mmhg IS NULL OR pressure_mmhg BETWEEN 0 AND 400", name="pressure_range"),
        CheckConstraint(
            "(pressure_mmhg IS NULL AND pressure_unit IS NULL) OR "
            "(pressure_mmhg IS NOT NULL AND pressure_unit IS NOT NULL AND pressure_unit = 'mmHg')",
            name="pressure_unit_explicit",
        ),
        Index("ix_acoustic_events_session_received", "session_id", "received_at"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    event_id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), nullable=False)
    batch_row_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("telemetry_batches.id", ondelete="CASCADE"), nullable=False
    )
    device_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("devices.id", ondelete="RESTRICT"), nullable=False
    )
    session_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("measurement_sessions.id", ondelete="CASCADE"), nullable=False
    )
    event_sequence: Mapped[int | None] = mapped_column(BigInteger)
    event_type: Mapped[str] = mapped_column(String(48), nullable=False)
    device_timestamp_ms: Mapped[int | None] = mapped_column(BigInteger)
    device_recorded_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    strength: Mapped[Decimal | None] = mapped_column(Numeric(12, 4))
    pressure_mmhg: Mapped[Decimal | None] = mapped_column(Numeric(7, 2))
    pressure_unit: Mapped[str | None] = mapped_column(String(8))
    is_valid: Mapped[bool] = mapped_column(Boolean, nullable=False, default=True)
    quality_flags: Mapped[list[str]] = mapped_column(JSON, nullable=False, default=list)
    firmware_version: Mapped[str | None] = mapped_column(String(64))
    processing_algorithm_version: Mapped[str | None] = mapped_column(String(64))
    data_provenance: Mapped[str] = mapped_column(String(32), nullable=False)
    metadata_json: Mapped[dict[str, object]] = mapped_column(JSON, nullable=False, default=dict)

    session: Mapped["MeasurementSession"] = relationship(back_populates="acoustic_events")
