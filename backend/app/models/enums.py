"""Persisted lifecycle vocabulary for the current data model."""

from enum import Enum


class SessionLifecycle(str, Enum):
    CREATED = "created"
    STARTING = "starting"
    MEASURING = "measuring"
    STOPPING = "stopping"
    COMPLETED = "completed"
    CANCELLED = "cancelled"
    INTERRUPTED = "interrupted"
    ERROR = "error"


class ReportStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"


class DeviceCommandStatus(str, Enum):
    CREATED = "created"
    SENT = "sent"
    ACCEPTED = "accepted"
    REJECTED = "rejected"
    COMPLETED = "completed"
    TIMED_OUT = "timed_out"
    FAILED = "failed"
