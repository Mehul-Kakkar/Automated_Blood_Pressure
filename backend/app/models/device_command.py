"""Auditable device command state; command payloads and credentials are not stored."""

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import CheckConstraint, DateTime, ForeignKey, Index, String, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column
from sqlalchemy.types import Uuid

from app.db.base import Base
from app.models.enums import DeviceCommandStatus

COMMAND_STATUSES = tuple(status.value for status in DeviceCommandStatus)
COMMAND_STATUS_SQL = ", ".join(f"'{status}'" for status in COMMAND_STATUSES)


class DeviceCommand(Base):
    __tablename__ = "device_commands"
    __table_args__ = (
        UniqueConstraint("command_id", name="uq_device_commands_command_id"),
        CheckConstraint(f"status IN ({COMMAND_STATUS_SQL})", name="status_valid"),
        CheckConstraint("command_type IN ('START', 'STOP', 'STATUS_REQUEST')", name="command_type_valid"),
        Index("ix_device_commands_device_status", "device_id", "status"),
        Index("ix_device_commands_expires_status", "expires_at", "status"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    command_id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), nullable=False)
    device_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("devices.id", ondelete="RESTRICT"), nullable=False
    )
    session_id: Mapped[UUID | None] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("measurement_sessions.id", ondelete="SET NULL")
    )
    command_type: Mapped[str] = mapped_column(String(24), nullable=False)
    status: Mapped[str] = mapped_column(String(16), nullable=False, default=DeviceCommandStatus.CREATED.value)
    issued_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    sent_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    response_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    completed_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    response_code: Mapped[str | None] = mapped_column(String(64))
    response_state: Mapped[str | None] = mapped_column(String(24))
