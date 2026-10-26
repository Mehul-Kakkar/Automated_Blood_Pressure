"""Non-sensitive MQTT transport health response."""

from datetime import datetime
from typing import Literal

from pydantic import BaseModel


class MessagingHealthResponse(BaseModel):
    state: Literal["disabled", "disconnected", "connected"]
    connected: bool
    queue_depth: int
    queue_capacity: int
    last_connection_at: datetime | None
    last_message_at: datetime | None
    disconnect_count: int
    accepted_count: int
    rejected_count: int
    oversize_count: int
    queue_full_count: int
    processing_error_count: int
