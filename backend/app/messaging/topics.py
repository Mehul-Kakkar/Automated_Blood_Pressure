"""Versioned MQTT topic construction and strict parser."""

import re
from dataclasses import dataclass
from uuid import UUID

TOPIC_ROOT = "bp/devices"
LEGACY_TOPIC_ROOT = "bp/v1/devices"
_TOPIC_PATTERN = re.compile(
    r"^bp/devices/(?P<device_id>[A-Za-z0-9][A-Za-z0-9_.-]{0,127})/(?P<kind>telemetry|status|commands|ack)$"
)
_LEGACY_TOPIC_PATTERN = re.compile(
    r"^bp/v1/devices/(?P<device_id>[0-9a-fA-F-]{36})/(?P<kind>telemetry|status|commands|responses)$"
)


@dataclass(frozen=True)
class DeviceTopic:
    device_id: str
    kind: str
    legacy: bool = False


def device_topic(device_id: str | UUID, kind: str) -> str:
    device_key = str(device_id)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", device_key):
        raise ValueError("Device topic identity contains unsupported characters.")
    if kind not in {"telemetry", "status", "commands", "ack"}:
        raise ValueError("Unsupported device topic kind.")
    return f"{TOPIC_ROOT}/{device_key}/{kind}"


def legacy_device_topic(device_id: UUID, kind: str) -> str:
    if kind not in {"telemetry", "status", "commands", "responses"}:
        raise ValueError("Unsupported legacy device topic kind.")
    return f"{LEGACY_TOPIC_ROOT}/{device_id}/{kind}"


def parse_device_topic(topic: str) -> DeviceTopic:
    match = _TOPIC_PATTERN.fullmatch(topic)
    if match is not None:
        return DeviceTopic(device_id=match.group("device_id"), kind=match.group("kind"))
    legacy_match = _LEGACY_TOPIC_PATTERN.fullmatch(topic)
    if legacy_match is not None:
        try:
            device_id = str(UUID(legacy_match.group("device_id")))
        except ValueError as exc:
            raise ValueError("Legacy topic contains an invalid device UUID.") from exc
        return DeviceTopic(device_id=device_id, kind=legacy_match.group("kind"), legacy=True)
    raise ValueError("Topic is outside the supported MQTT namespaces.")
