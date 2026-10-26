"""Minimal JSON logging with an explicit allowlist of log fields."""

import json
import logging
from datetime import datetime, timezone

from app.core.middleware import request_id_var


class JsonFormatter(logging.Formatter):
    """Emit stable JSON records and omit arbitrary messages/exception text."""

    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, object] = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "level": record.levelname.lower(),
            "logger": record.name,
            "event": getattr(record, "event", "application_event"),
            "request_id": getattr(record, "request_id", request_id_var.get()),
        }
        for key in ("method", "path", "status_code", "duration_ms", "error_code"):
            value = getattr(record, key, None)
            if value is not None:
                payload[key] = value
        return json.dumps(payload, separators=(",", ":"), ensure_ascii=True)


def configure_logging(level: str) -> None:
    handler = logging.StreamHandler()
    handler.setFormatter(JsonFormatter())
    root = logging.getLogger()
    root.handlers = [handler]
    root.setLevel(level.upper())
