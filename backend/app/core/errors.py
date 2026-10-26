"""Consistent, non-sensitive API error responses."""

import logging
from typing import Any

from app.core.middleware import request_id_var
from app.schemas.errors import ApiError, ErrorDetail
from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

logger = logging.getLogger("api.errors")


def _response(
    status_code: int,
    code: str,
    message: str,
    details: list[ErrorDetail] | None = None,
    request_id: str | None = None,
) -> JSONResponse:
    correlation_id = request_id or request_id_var.get()
    body = ApiError(
        error={"code": code, "message": message, "details": details or [], "request_id": correlation_id}
    )
    return JSONResponse(
        status_code=status_code,
        content=body.model_dump(),
        headers={"X-Request-ID": correlation_id},
    )


def register_exception_handlers(application: FastAPI) -> None:
    @application.exception_handler(RequestValidationError)
    async def validation_error_handler(request: Request, exc: RequestValidationError) -> JSONResponse:
        # Keep field locations and error types; never echo rejected input values.
        details = [
            ErrorDetail(
                location=".".join(str(part) for part in error.get("loc", ())),
                issue=str(error.get("type", "invalid")),
                message="Invalid or missing value.",
            )
            for error in exc.errors()
        ]
        return _response(
            422, "request_validation_error", "Request validation failed.", details,
            request_id=getattr(request.state, "request_id", None),
        )

    @application.exception_handler(StarletteHTTPException)
    async def http_error_handler(request: Request, exc: StarletteHTTPException) -> JSONResponse:
        messages = {
            401: "Authentication is required or invalid.",
            403: "The request is not authorized.",
            404: "The requested resource was not found.",
            405: "The method is not allowed.",
            409: "The request conflicts with the current device or session state.",
            503: "The requested service is temporarily unavailable.",
        }
        codes = {401: "unauthorized", 403: "forbidden", 404: "not_found", 409: "state_conflict", 503: "service_unavailable"}
        code = codes.get(exc.status_code, "http_error")
        return _response(
            exc.status_code,
            code,
            messages.get(exc.status_code, "The request could not be completed."),
            request_id=getattr(request.state, "request_id", None),
        )

    @application.exception_handler(Exception)
    async def unexpected_error_handler(request: Request, exc: Exception) -> JSONResponse:
        correlation_id = getattr(request.state, "request_id", "-")
        logger.error(
            "unhandled_exception",
            extra={"event": "unhandled_exception", "error_code": "internal_error", "request_id": correlation_id},
        )
        return _response(500, "internal_error", "An unexpected server error occurred.", request_id=correlation_id)
