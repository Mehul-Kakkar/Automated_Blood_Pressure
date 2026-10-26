"""ASGI entry point for the blood-pressure prototype API."""

from contextlib import asynccontextmanager

from app.api.router import api_router
from app.core.config import get_settings
from app.core.errors import register_exception_handlers
from app.core.logging import configure_logging
from app.core.middleware import RequestContextMiddleware
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from app.db.session import get_session_factory
from app.messaging.service import MessagingService


def create_app() -> FastAPI:
    """Build an application instance using the current environment settings."""
    settings = get_settings()
    configure_logging(settings.log_level)

    @asynccontextmanager
    async def lifespan(application: FastAPI):
        session_factory = get_session_factory() if settings.mqtt_configured else None
        messaging = MessagingService(settings, session_factory)
        application.state.messaging = messaging
        messaging.start()
        try:
            yield
        finally:
            messaging.stop()

    application = FastAPI(
        title=settings.app_name,
        version="0.1.0",
        description="Foundational API for an experimental device project.",
        docs_url="/docs" if settings.docs_enabled else None,
        redoc_url="/redoc" if settings.docs_enabled else None,
        lifespan=lifespan,
    )
    application.state.settings = settings
    application.add_middleware(RequestContextMiddleware)
    if settings.allowed_origins:
        application.add_middleware(
            CORSMiddleware,
            allow_origins=settings.allowed_origins,
            allow_credentials=False,
            allow_methods=["GET", "POST", "OPTIONS"],
            allow_headers=["Accept", "Authorization", "Content-Type", "X-Request-ID"],
            expose_headers=["X-Request-ID"],
        )
    application.include_router(api_router)
    register_exception_handlers(application)
    return application


app = create_app()
