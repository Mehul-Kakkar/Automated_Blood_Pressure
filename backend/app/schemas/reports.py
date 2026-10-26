"""Typed report API representations."""

from datetime import datetime
from typing import Any
from uuid import UUID

from pydantic import BaseModel


class ReportResponse(BaseModel):
    id: UUID
    session_id: UUID
    report_version: str
    generator_version: str
    status: str
    validation_status: str | None
    sample_count: int | None
    content_sha256: str | None
    generated_at: datetime | None
    report: dict[str, Any] | None = None
