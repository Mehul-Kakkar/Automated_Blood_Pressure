"""Health and readiness response models."""

from typing import Literal

from pydantic import BaseModel


class HealthResponse(BaseModel):
    status: Literal["ok"]
    service: str


class DatabaseReadiness(BaseModel):
    status: Literal["available", "unavailable", "not_configured", "not_checked"]


class ReadinessResponse(BaseModel):
    status: Literal["ready", "not_ready"]
    database: DatabaseReadiness
