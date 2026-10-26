"""Persisted measurement session lifecycle metadata."""

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import CheckConstraint, DateTime, ForeignKey, Index, String, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column, relationship
from sqlalchemy.types import Uuid

from app.db.base import Base
from app.models.enums import SessionLifecycle

SESSION_STATES = tuple(state.value for state in SessionLifecycle)
SESSION_STATE_SQL = ", ".join(f"'{state}'" for state in SESSION_STATES)


class MeasurementSession(Base):
    __tablename__ = "measurement_sessions"
    __table_args__ = (
        UniqueConstraint("device_id", "client_request_id", name="uq_measurement_sessions_device_request"),
        CheckConstraint(f"lifecycle IN ({SESSION_STATE_SQL})", name="lifecycle_valid"),
        CheckConstraint("ended_at IS NULL OR started_at IS NULL OR ended_at >= started_at", name="time_order"),
        CheckConstraint(
            "lifecycle != 'completed' OR (started_at IS NOT NULL AND ended_at IS NOT NULL)",
            name="completed_requires_confirmed_times",
        ),
        Index("ix_measurement_sessions_device_created", "device_id", "created_at"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    device_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("devices.id", ondelete="RESTRICT"), nullable=False
    )
    client_request_id: Mapped[UUID | None] = mapped_column(Uuid(as_uuid=True))
    lifecycle: Mapped[str] = mapped_column(String(24), nullable=False, default=SessionLifecycle.CREATED.value)
    protocol_version: Mapped[str | None] = mapped_column(String(32))
    firmware_version: Mapped[str | None] = mapped_column(String(64))
    processing_algorithm_version: Mapped[str | None] = mapped_column(String(64))
    data_provenance: Mapped[str] = mapped_column(String(32), nullable=False, default="device_unspecified")
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False
    )
    started_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    ended_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    termination_reason: Mapped[str | None] = mapped_column(String(128))

    device: Mapped["Device"] = relationship(back_populates="sessions")
    batches: Mapped[list["TelemetryBatch"]] = relationship(back_populates="session", cascade="all, delete-orphan")
    samples: Mapped[list["TelemetrySample"]] = relationship(back_populates="session", cascade="all, delete-orphan")
    acoustic_events: Mapped[list["AcousticEvent"]] = relationship(back_populates="session", cascade="all, delete-orphan")
    reports: Mapped[list["Report"]] = relationship(back_populates="session", cascade="all, delete-orphan")
