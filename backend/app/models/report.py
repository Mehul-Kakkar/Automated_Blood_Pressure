"""Report generation metadata only; report generation is not implemented."""

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import BigInteger, CheckConstraint, DateTime, ForeignKey, Integer, JSON, String, UniqueConstraint, func
from sqlalchemy.orm import Mapped, mapped_column, relationship
from sqlalchemy.types import Uuid

from app.db.base import Base
from app.models.enums import ReportStatus


class Report(Base):
    __tablename__ = "reports"
    __table_args__ = (
        UniqueConstraint("session_id", "report_version", name="uq_reports_session_id_report_version"),
        CheckConstraint("status IN ('pending', 'running', 'completed', 'failed')", name="status_valid"),
        CheckConstraint("sample_count IS NULL OR sample_count >= 0", name="sample_count_nonnegative"),
    )

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    session_id: Mapped[UUID] = mapped_column(
        Uuid(as_uuid=True), ForeignKey("measurement_sessions.id", ondelete="CASCADE"), nullable=False
    )
    report_version: Mapped[str] = mapped_column(String(32), nullable=False)
    generator_version: Mapped[str] = mapped_column(String(64), nullable=False)
    status: Mapped[str] = mapped_column(String(16), nullable=False, default=ReportStatus.PENDING.value)
    validation_status: Mapped[str | None] = mapped_column(String(32))
    sample_count: Mapped[int | None] = mapped_column(Integer)
    input_watermark_sequence: Mapped[int | None] = mapped_column(BigInteger)
    failure_code: Mapped[str | None] = mapped_column(String(64))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now(), nullable=False
    )
    generated_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    report_data: Mapped[dict[str, object] | None] = mapped_column(JSON)
    content_sha256: Mapped[str | None] = mapped_column(String(64))

    session: Mapped["MeasurementSession"] = relationship(back_populates="reports")
