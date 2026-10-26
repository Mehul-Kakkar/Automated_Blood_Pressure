"""Versioned MQTT topic construction and strict parser."""

import re
from dataclasses import dataclass
from uuid import UUID

TOPIC_ROOT = "bp/v1/devices"
_TOPIC_PATTERN = re.compile(
    r"^bp/v1/devices/(?P<device_id>[0-9a-fA-F-]{36})/(?P<kind>telemetry|status|commands|responses)$"
)


@dataclass(frozen=True)
class DeviceTopic:
    device_id: UUID
    kind: str


def device_topic(device_id: UUID, kind: str) -> str:
    if kind not in {"telemetry", "status", "commands", "responses"}:
        raise ValueError("Unsupported device topic kind.")
    return f"{TOPIC_ROOT}/{device_id}/{kind}"


def parse_device_topic(topic: str) -> DeviceTopic:
    match = _TOPIC_PATTERN.fullmatch(topic)
    if match is None:
        raise ValueError("Topic is outside the supported bp/v1 namespace.")
    return DeviceTopic(device_id=UUID(match.group("device_id")), kind=match.group("kind"))
