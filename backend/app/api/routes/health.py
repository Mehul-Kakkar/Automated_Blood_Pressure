"""Liveness and optional database readiness probes."""

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from app.core.config import Settings
from app.schemas.health import DatabaseReadiness, HealthResponse, ReadinessResponse
from app.schemas.messaging import MessagingHealthResponse
from app.services.database import check_database as probe_database_connection

router = APIRouter()


@router.get("/health", response_model=HealthResponse, summary="Check API liveness")
def health(request: Request) -> HealthResponse:
    settings = request.app.state.settings
    return HealthResponse(status="ok", service=settings.app_name)


@router.get("/ready", response_model=ReadinessResponse, summary="Check API and configured dependencies")
def readiness(request: Request, check_database: bool = True) -> ReadinessResponse | JSONResponse:
    settings = request.app.state.settings
    if not settings.database_configured:
        return ReadinessResponse(
            status="ready", database=DatabaseReadiness(status="not_configured"),
        )
    if not check_database:
        return ReadinessResponse(
            status="ready", database=DatabaseReadiness(status="not_checked"),
        )
    if probe_database_connection(settings):
        return ReadinessResponse(status="ready", database=DatabaseReadiness(status="available"))
    return JSONResponse(
        status_code=503,
        content=ReadinessResponse(
            status="not_ready", database=DatabaseReadiness(status="unavailable")
        ).model_dump(),
    )


@router.get("/health/messaging", response_model=MessagingHealthResponse, summary="Check MQTT transport health")
def messaging_health(request: Request) -> MessagingHealthResponse:
    service = getattr(request.app.state, "messaging", None)
    if service is None:
        return MessagingHealthResponse(
            state="disabled", connected=False, queue_depth=0, queue_capacity=0,
            last_connection_at=None, last_message_at=None, disconnect_count=0,
            accepted_count=0, rejected_count=0, oversize_count=0,
            queue_full_count=0, processing_error_count=0,
        )
    return MessagingHealthResponse.model_validate(service.health)
