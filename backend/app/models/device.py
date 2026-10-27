"""Registered physical devices."""

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import Boolean, CheckConstraint, DateTime, ForeignKey, Index, Integer, String, func
from sqlalchemy.orm import Mapped, mapped_column, relationship
from sqlalchemy.types import Uuid

from app.db.base import Base


class Device(Base):
    __tablename__ = "devices"
    __table_args__ = (
        Index("ix_devices_owner_username", "owner_username"),
        CheckConstraint(
            "pressure_calibration_confirmed = false OR pressure_calibration_version IS NOT NULL",
            name="pressure_calibration_requires_version",
        ),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    owner_username: Mapped[str | None] = mapped_column(String(48), ForeignKey("owners.username", ondelete="SET NULL"))
    device_identifier: Mapped[str] = mapped_column(String(128), unique=True, nullable=False)
    hardware_revision: Mapped[str | None] = mapped_column(String(64))
    firmware_version: Mapped[str | None] = mapped_column(String(64))
    processing_algorithm_version: Mapped[str | None] = mapped_column(String(64))
    mqtt_enabled: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False, server_default="false")
    mqtt_principal: Mapped[str | None] = mapped_column(String(128), unique=True)
    reported_state: Mapped[str | None] = mapped_column(String(24))
    state_reported_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    reported_lost_sample_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0, server_default="0")
    pressure_calibration_confirmed: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False, server_default="false")
    pressure_calibration_version: Mapped[str | None] = mapped_column(String(64))
    registered_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))

    sessions: Mapped[list["MeasurementSession"]] = relationship(back_populates="device")
    owner: Mapped["Owner | None"] = relationship(back_populates="devices")
