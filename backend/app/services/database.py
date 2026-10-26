"""Optional SQLAlchemy database connectivity probe; no session persistence yet."""

import logging

from sqlalchemy import text
from sqlalchemy.exc import SQLAlchemyError

from app.core.config import Settings
from app.db.session import create_database_engine

logger = logging.getLogger("api.database")


def check_database(settings: Settings) -> bool:
    """Open a short-lived connection and run SELECT 1 without logging the URL."""
    try:
        if settings.database_url is None:
            return False
        engine = create_database_engine(settings)
        try:
            with engine.connect() as connection:
                connection.execute(text("SELECT 1"))
            return True
        finally:
            engine.dispose()
    except (SQLAlchemyError, ValueError, TypeError):
        logger.warning("database_readiness_failed", extra={"event": "database_readiness_failed"})
        return False
