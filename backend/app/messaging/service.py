"""MQTT lifecycle, identity validation, bounded ingestion, and command handling."""

import hashlib
import json
import logging
import queue
import threading
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Callable, Protocol
from uuid import UUID, uuid5

from sqlalchemy import func, select, update
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session, sessionmaker

from app.core.config import Settings
from app.messaging.schemas import (
    DeviceCommandRequest,
    DeviceCommandResponse,
    DeviceCommandType,
    DeviceReportedState,
    DeviceStatusMessage,
    TelemetryEnvelope,
)
from app.messaging.topics import DeviceTopic, device_topic, legacy_device_topic, parse_device_topic
from app.messaging.transport import IncomingMessage, MQTTTransport, PahoMQTTTransport
from app.models import Device, DeviceCommand, DeviceCommandStatus, MeasurementSession, SessionEvent, TelemetrySample
from app.schemas.telemetry import TelemetryBatchInput
from app.services.telemetry import ingest_telemetry_batch
from app.services.session_lifecycle import make_command, record_session_event

logger = logging.getLogger("api.messaging")


def _payload_digest(body: dict[str, object]) -> str:
    canonical = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _as_utc(value: datetime) -> datetime:
    """Normalize database timestamps that may be naive on SQLite to UTC."""
    if value.tzinfo is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


class MessagingError(Exception):
    """Expected MQTT transport, identity, payload, or command rejection."""


class MessageRejected(MessagingError):
    pass


class DeviceIdentityRejected(MessageRejected):
    pass


class CommandNotAuthorized(MessagingError):
    pass


class CommandExpired(MessagingError):
    pass


class DuplicateCommandId(MessagingError):
    pass


class BrokerUnavailable(MessagingError):
    pass


class CommandDeliveryUncertain(BrokerUnavailable):
    """Publish was attempted, but transport failure prevents a delivery conclusion."""


class CommandAuthorizer(Protocol):
    def authorize(self, command: DeviceCommandRequest) -> bool: ...


class DenyAllCommandAuthorizer:
    """Fail closed until authenticated REST command authorization is wired."""

    def authorize(self, command: DeviceCommandRequest) -> bool:
        return False


class DeviceRegistry(Protocol):
    def __call__(self, db: Session, device_id: UUID) -> Device | None: ...


@dataclass(frozen=True)
class QueuedMessage:
    topic: str
    payload: bytes
    qos: int
    mid: int
    retained: bool = False


class MessagingService:
    SUBSCRIPTIONS = [
        ("bp/devices/+/telemetry", 1),
        ("bp/devices/+/status", 1),
        ("bp/devices/+/ack", 1),
    ]
    LEGACY_SUBSCRIPTIONS = [
        ("bp/v1/devices/+/telemetry", 1),
        ("bp/v1/devices/+/status", 1),
        ("bp/v1/devices/+/responses", 1),
    ]

    def __init__(
        self,
        settings: Settings,
        session_factory: sessionmaker[Session] | None,
        *,
        transport_factory: Callable[[], MQTTTransport] | None = None,
        command_authorizer: CommandAuthorizer | None = None,
    ) -> None:
        self.settings = settings
        self.session_factory = session_factory
        self.transport_factory = transport_factory or (lambda: PahoMQTTTransport(settings))
        self.command_authorizer = command_authorizer or DenyAllCommandAuthorizer()
        self._queue: queue.Queue[QueuedMessage] = queue.Queue(maxsize=settings.mqtt_queue_size)
        self._client: MQTTTransport | None = None
        self._subscription_mids: dict[int, tuple[str, ...]] = {}
        self._worker: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._legacy_device_ids: set[UUID] = set()
        self._enabled = settings.mqtt_configured
        self._connected = False
        self._last_connection_at: datetime | None = None
        self._last_message_at: datetime | None = None
        self._disconnect_count = 0
        self._accepted_count = 0
        self._rejected_count = 0
        self._oversize_count = 0
        self._queue_full_count = 0
        self._processing_error_count = 0
        self._started = False

    @property
    def health(self) -> dict[str, object]:
        with self._lock:
            if not self._enabled:
                state = "disabled"
            elif self._connected:
                state = "connected"
            else:
                state = "disconnected"
            return {
                "state": state,
                "connected": self._connected,
                "queue_depth": self._queue.qsize(),
                "queue_capacity": self.settings.mqtt_queue_size,
                "last_connection_at": self._last_connection_at,
                "last_message_at": self._last_message_at,
                "disconnect_count": self._disconnect_count,
                "accepted_count": self._accepted_count,
                "rejected_count": self._rejected_count,
                "oversize_count": self._oversize_count,
                "queue_full_count": self._queue_full_count,
                "processing_error_count": self._processing_error_count,
            }

    def start(self) -> None:
        if not self._enabled:
            return
        if self.session_factory is None:
            raise RuntimeError("MQTT requires a configured database session factory.")
        with self._lock:
            if self._started:
                return
            self._started = True
        try:
            client = self.transport_factory()
            client.on_connect = self._on_connect
            client.on_disconnect = self._on_disconnect
            client.on_message = self._on_message
            if hasattr(client, "on_subscribe"):
                client.on_subscribe = self._on_subscribe
            self._client = client
            self._worker = threading.Thread(target=self._worker_loop, name="mqtt-ingestion", daemon=True)
            self._worker.start()
            client.connect_async()
            client.loop_start()
        except Exception:
            with self._lock:
                self._connected = False
            logger.error("mqtt_start_failed", extra={"event": "mqtt_start_failed"})

    def stop(self) -> None:
        self._stop_event.set()
        client = self._client
        if client is not None:
            try:
                client.disconnect()
            except Exception:
                logger.warning("mqtt_disconnect_failed", extra={"event": "mqtt_disconnect_failed"})
            try:
                client.loop_stop()
            except Exception:
                logger.warning("mqtt_loop_stop_failed", extra={"event": "mqtt_loop_stop_failed"})
        if self._worker is not None:
            self._worker.join(timeout=2)

    def _on_connect(self, client: MQTTTransport, _userdata: object, _flags: object, reason_code: object, _properties: object) -> None:
        is_failure = getattr(reason_code, "is_failure", None)
        if is_failure is None:
            try:
                success = int(reason_code) == 0  # type: ignore[arg-type]
            except (TypeError, ValueError):
                success = False
        else:
            success = not is_failure
        if not success:
            with self._lock:
                self._connected = False
            logger.warning("mqtt_connect_rejected", extra={"event": "mqtt_connect_rejected"})
            return
        subscriptions = self.SUBSCRIPTIONS + (
            self.LEGACY_SUBSCRIPTIONS if self.settings.mqtt_legacy_topic_compatibility else []
        )
        result = client.subscribe(subscriptions)
        rc = result[0] if isinstance(result, tuple) else result
        if rc not in (0, None):
            with self._lock:
                self._connected = False
            logger.warning("mqtt_subscribe_failed", extra={"event": "mqtt_subscribe_failed"})
            return
        if isinstance(result, tuple) and len(result) > 1:
            with self._lock:
                self._subscription_mids[int(result[1])] = tuple(topic for topic, _qos in subscriptions)
        with self._lock:
            self._connected = True
            self._last_connection_at = datetime.now(timezone.utc)

    def _on_subscribe(
        self, _client: MQTTTransport, _userdata: object, mid: int, reason_codes: object, _properties: object
    ) -> None:
        codes = list(reason_codes) if isinstance(reason_codes, (list, tuple)) else []
        with self._lock:
            filters = self._subscription_mids.pop(int(mid), ())
        failures = []
        for index, code in enumerate(codes):
            is_failure = getattr(code, "is_failure", None)
            if is_failure is None:
                try:
                    is_failure = int(code) >= 128
                except (TypeError, ValueError):
                    is_failure = True
            if is_failure:
                failures.append({"filter": filters[index] if index < len(filters) else "unknown", "reason": str(code)})
        if failures:
            logger.error(
                "mqtt_subscription_rejected",
                extra={"event": "mqtt_subscription_rejected", "rejected_filters": failures},
            )
            if len(failures) == len(filters) and filters:
                with self._lock:
                    self._connected = False

    def _on_disconnect(self, _client: MQTTTransport, _userdata: object, *_args: object) -> None:
        with self._lock:
            if self._connected:
                self._disconnect_count += 1
            self._connected = False

    def _on_message(self, client: MQTTTransport, _userdata: object, message: IncomingMessage) -> None:
        payload = bytes(message.payload)
        if len(payload) > self.settings.mqtt_max_payload_bytes:
            with self._lock:
                self._oversize_count += 1
                self._rejected_count += 1
            logger.warning("mqtt_payload_too_large", extra={"event": "mqtt_payload_too_large"})
            client.ack(message.mid, message.qos)
            return
        item = QueuedMessage(message.topic, payload, message.qos, message.mid, bool(getattr(message, "retain", False)))
        try:
            self._queue.put_nowait(item)
        except queue.Full:
            with self._lock:
                self._queue_full_count += 1
            # Manual QoS 1 ack is intentionally withheld so the broker can redeliver.
            logger.warning("mqtt_ingestion_queue_full", extra={"event": "mqtt_ingestion_queue_full"})

    def _worker_loop(self) -> None:
        next_expiry_check = 0.0
        while not self._stop_event.is_set():
            try:
                item = self._queue.get(timeout=0.25)
            except queue.Empty:
                item = None
            if item is not None:
                self._process_queued(item)
            if self._enabled and self.session_factory is not None:
                import time

                if time.monotonic() >= next_expiry_check:
                    try:
                        self.expire_commands()
                    except Exception:
                        with self._lock:
                            self._processing_error_count += 1
                        logger.warning("mqtt_expiry_sweep_failed", extra={"event": "mqtt_expiry_sweep_failed"})
                    next_expiry_check = time.monotonic() + 1.0

    def process_one(self, timeout: float = 0.0) -> bool:
        """Process one queued message synchronously; useful for deterministic tests."""
        try:
            item = self._queue.get(timeout=timeout)
        except queue.Empty:
            return False
        self._process_queued(item)
        return True

    def _process_queued(self, item: QueuedMessage) -> None:
        try:
            self._handle_message(item)
        except MessageRejected as exc:
            with self._lock:
                self._rejected_count += 1
            if self._client is not None:
                self._client.ack(item.mid, item.qos)
            logger.warning(
                "mqtt_message_rejected",
                extra={"event": "mqtt_message_rejected", "reason": str(exc)},
            )
        except Exception:
            # Preserve broker redelivery by not acknowledging a transient database failure.
            with self._lock:
                self._processing_error_count += 1
            logger.warning("mqtt_message_processing_failed", extra={"event": "mqtt_message_processing_failed"})
        else:
            with self._lock:
                self._accepted_count += 1
                self._last_message_at = datetime.now(timezone.utc)
            if self._client is not None:
                self._client.ack(item.mid, item.qos)
        finally:
            self._queue.task_done()

    def _handle_message(self, item: QueuedMessage) -> None:
        try:
            topic = parse_device_topic(item.topic)
            body = json.loads(item.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise MessageRejected("Malformed topic or JSON payload.") from exc
        if not isinstance(body, dict):
            raise MessageRejected("MQTT payload must be a JSON object.")
        if item.retained:
            raise MessageRejected("Retained device messages are not accepted as live telemetry or status.")
        if topic.legacy:
            with self._lock:
                self._legacy_device_ids.add(UUID(topic.device_id))
        if topic.kind == "telemetry":
            self._handle_telemetry(topic, body)
        elif topic.kind == "status":
            self._handle_status(topic, body)
        elif topic.kind in {"ack", "responses"}:
            self._handle_response(topic, body)
        else:
            raise MessageRejected("Backend does not accept messages on command topics.")

    def _require_enrolled_device(
        self, db: Session, topic: DeviceTopic, payload_device_id: UUID, *, lock: bool = False
    ) -> Device:
        if topic.legacy:
            try:
                legacy_id = UUID(topic.device_id)
            except ValueError as exc:
                raise DeviceIdentityRejected("Legacy MQTT topic does not contain a device UUID.") from exc
            statement = select(Device).where(Device.id == legacy_id)
        else:
            statement = select(Device).where(Device.mqtt_principal == topic.device_id)
        if lock:
            statement = statement.with_for_update()
        device = db.scalar(statement)
        if (
            device is None
            or not device.mqtt_enabled
            or device.id != payload_device_id
            or (not topic.legacy and device.mqtt_principal != topic.device_id)
        ):
            raise DeviceIdentityRejected("Device is not enrolled for MQTT messaging.")
        return device

    def _handle_telemetry(self, topic: DeviceTopic, body: dict[str, object]) -> None:
        if self.session_factory is None:
            raise RuntimeError("Database is unavailable.")
        try:
            envelope = TelemetryEnvelope.model_validate(body)
        except Exception as exc:
            raise MessageRejected("Telemetry schema validation failed.") from exc
        now = datetime.now(timezone.utc)
        with self.session_factory() as db:
            device = self._require_enrolled_device(db, topic, envelope.payload.device_id)
            if any(sample.pressure_mmhg is not None for sample in envelope.payload.samples) and not (
                device.pressure_calibration_confirmed and device.pressure_calibration_version
            ):
                raise MessageRejected("Pressure telemetry requires an explicitly confirmed device calibration.")
            samples_by_device_time = {
                sample.device_timestamp_ms: sample.pressure_mmhg
                for sample in envelope.payload.samples
                if sample.device_timestamp_ms is not None and sample.pressure_mmhg is not None
            }
            for event in envelope.payload.acoustic_events:
                if event.pressure_mmhg is None:
                    continue
                if event.device_timestamp_ms is None or samples_by_device_time.get(event.device_timestamp_ms) != event.pressure_mmhg:
                    raise MessageRejected("Event pressure must match a pressure sample at the same device timestamp.")
            pressure_calibration_version = device.pressure_calibration_version
            existing_max = db.scalar(
                select(func.max(TelemetrySample.sample_sequence)).where(
                    TelemetrySample.session_id == envelope.payload.session_id
                )
            )
            previous_max = existing_max
            for sample in envelope.payload.samples:
                if sample.sample_sequence is not None:
                    if previous_max is not None and sample.sample_sequence < previous_max:
                        sample.quality_flags = list(dict.fromkeys([*sample.quality_flags, "out_of_order_sequence"]))
                    previous_max = max(previous_max or sample.sample_sequence, sample.sample_sequence)
                if sample.device_recorded_at is not None:
                    skew = (sample.device_recorded_at - now).total_seconds()
                    if abs(skew) > 300:
                        sample.quality_flags = list(dict.fromkeys([*sample.quality_flags, "device_clock_skew_suspected"]))
        with self.session_factory() as db:
            ingest_telemetry_batch(
                db, envelope.payload, pressure_calibration_version=pressure_calibration_version
            )
        with self.session_factory() as db:
            with db.begin():
                device = self._require_enrolled_device(db, topic, envelope.payload.device_id)
                device.last_seen_at = datetime.now(timezone.utc)
                if envelope.payload.firmware_version:
                    device.firmware_version = envelope.payload.firmware_version
                if envelope.payload.processing_algorithm_version:
                    device.processing_algorithm_version = envelope.payload.processing_algorithm_version

    def _handle_status(self, topic: DeviceTopic, body: dict[str, object]) -> None:
        if self.session_factory is None:
            raise RuntimeError("Database is unavailable.")
        try:
            status = DeviceStatusMessage.model_validate(body)
        except Exception as exc:
            raise MessageRejected("Status schema validation failed.") from exc
        if status.start_request_id is not None:
            self._handle_device_start_request(topic, status)
        with self.session_factory() as db:
            with db.begin():
                device = self._require_enrolled_device(db, topic, status.device_id, lock=True)
                measurement_session = None
                if status.session_id is not None:
                    measurement_session = db.get(MeasurementSession, status.session_id)
                    if measurement_session is None or measurement_session.device_id != device.id:
                        raise DeviceIdentityRejected("Status session does not belong to the reporting device.")
                duplicate = db.scalar(
                    select(SessionEvent).where(SessionEvent.device_id == device.id, SessionEvent.event_id == status.message_id)
                )
                if duplicate is not None:
                    if duplicate.payload_sha256 and duplicate.payload_sha256 != _payload_digest(body):
                        raise MessageRejected("Status message ID was reused with different content.")
                    return
                prior_sequence = db.scalar(
                    select(func.max(SessionEvent.device_sequence)).where(
                        SessionEvent.device_id == device.id, SessionEvent.event_type == "device_status"
                    )
                )
                prior_status = db.scalar(
                    select(SessionEvent)
                    .where(SessionEvent.device_id == device.id, SessionEvent.event_type == "device_status")
                    .order_by(SessionEvent.id.desc())
                    .limit(1)
                )
                previous_uptime = prior_status.data.get("device_uptime_ms") if prior_status else None
                rebooted = bool(
                    status.device_uptime_ms is not None
                    and isinstance(previous_uptime, int)
                    and status.device_uptime_ms < previous_uptime
                )
                prior_state_at = device.state_reported_at
                if prior_state_at is not None and prior_state_at.tzinfo is None:
                    prior_state_at = prior_state_at.replace(tzinfo=timezone.utc)
                heartbeat_stale = prior_state_at is None or prior_state_at < datetime.now(timezone.utc) - timedelta(
                    seconds=self.settings.device_state_max_age_seconds
                )
                rebooted = rebooted or bool(
                    prior_sequence is not None
                    and status.sequence < prior_sequence
                    and status.device_uptime_ms is not None
                    and heartbeat_stale
                )
                out_of_order = prior_sequence is not None and status.sequence <= prior_sequence and not rebooted
                previous_lost_count = device.reported_lost_sample_count
                if not out_of_order:
                    device.reported_state = status.state.value
                    device.state_reported_at = datetime.now(timezone.utc)
                    if measurement_session is not None:
                        if status.state == DeviceReportedState.MEASURING and measurement_session.lifecycle in {"starting", "interrupted"}:
                            measurement_session.lifecycle = "measuring"
                            if measurement_session.started_at is None:
                                measurement_session.started_at = datetime.now(timezone.utc)
                        elif (
                            status.state in {DeviceReportedState.IDLE, DeviceReportedState.STOPPED}
                            and measurement_session.lifecycle in {"stopping", "interrupted"}
                            and measurement_session.started_at is not None
                        ):
                            measurement_session.lifecycle = "completed"
                            measurement_session.ended_at = datetime.now(timezone.utc)
                        elif (
                            status.state == DeviceReportedState.STOPPED
                            and measurement_session.lifecycle == "measuring"
                            and measurement_session.started_at is not None
                        ):
                            # A physical display may stop locally; only an
                            # explicit device STOPPED report ends the shared session.
                            measurement_session.lifecycle = "completed"
                            measurement_session.ended_at = datetime.now(timezone.utc)
                            measurement_session.termination_reason = "device_local_stop_confirmed"
                        elif status.state == DeviceReportedState.FAULT and measurement_session.lifecycle in {
                            "created", "starting", "measuring", "stopping"
                        }:
                            measurement_session.lifecycle = "interrupted"
                            measurement_session.termination_reason = "device_fault_reported"
                    device.reported_lost_sample_count = status.lost_sample_count
                device.last_seen_at = datetime.now(timezone.utc)
                if not out_of_order and status.firmware_version:
                    device.firmware_version = status.firmware_version
                if not out_of_order and status.processing_algorithm_version:
                    device.processing_algorithm_version = status.processing_algorithm_version
                lost_sample_delta = max(0, status.lost_sample_count - previous_lost_count) if not out_of_order else 0
                record_session_event(
                    db,
                    device_id=device.id,
                    session_id=status.session_id,
                    event_type="device_status",
                    event_id=status.message_id,
                    payload_sha256=_payload_digest(body),
                    device_sequence=status.sequence,
                    device_recorded_at=status.reported_at,
                    data={
                        "state": status.state.value,
                        "device_uptime_ms": status.device_uptime_ms,
                        "lost_sample_count": status.lost_sample_count,
                        "lost_sample_delta": lost_sample_delta,
                        "out_of_order": out_of_order,
                        "clock_skew_suspected": bool(status.reported_at and abs((status.reported_at - datetime.now(timezone.utc)).total_seconds()) > 300),
                    },
                )
                if lost_sample_delta > 0 or status.state == DeviceReportedState.DATA_GAP:
                    record_session_event(
                        db,
                        device_id=device.id,
                        session_id=status.session_id,
                        event_type="data_gap",
                        data={
                            "reported_lost_sample_count": status.lost_sample_count,
                            "newly_reported_lost_samples": lost_sample_delta,
                        },
                        event_id=uuid5(status.message_id, "data-gap"),
                        device_sequence=status.sequence,
                        device_recorded_at=status.reported_at,
                    )

    def _handle_device_start_request(self, topic: DeviceTopic, status: DeviceStatusMessage) -> None:
        """Create/reuse an owner-scoped session and dispatch its normal START command.

        MQTT ACL identity plus the existing trusted-enrollment row authorizes the
        device request. No owner credential or username is accepted from payload.
        """
        if self.session_factory is None or status.start_request_id is None:
            raise RuntimeError("Database or start request is unavailable.")
        dispatch_session_id: UUID | None = None
        dispatch_device_id: UUID | None = None
        owner_username: str | None = None
        request_event_id = uuid5(status.start_request_id, "device-start-request")
        active_lifecycles = {"created", "starting", "measuring", "stopping", "interrupted"}
        with self.session_factory() as db:
            with db.begin():
                device = self._require_enrolled_device(db, topic, status.device_id, lock=True)
                if not device.owner_username:
                    raise DeviceIdentityRejected("Device must be associated with an owner before it can request a session.")
                owner_username = device.owner_username
                by_request = db.scalar(
                    select(MeasurementSession).where(
                        MeasurementSession.device_id == device.id,
                        MeasurementSession.client_request_id == status.start_request_id,
                    )
                )
                request_event = db.scalar(
                    select(SessionEvent).where(
                        SessionEvent.device_id == device.id,
                        SessionEvent.event_id == request_event_id,
                    )
                )
                device_originated = request_event is not None
                session = by_request
                if session is None:
                    session = db.scalar(
                        select(MeasurementSession)
                        .where(
                            MeasurementSession.device_id == device.id,
                            MeasurementSession.lifecycle.in_(active_lifecycles),
                        )
                        .order_by(MeasurementSession.created_at.desc(), MeasurementSession.id.desc())
                        .limit(1)
                    )
                if session is None:
                    session = MeasurementSession(
                        device_id=device.id,
                        client_request_id=status.start_request_id,
                        lifecycle="created",
                        data_provenance="device_live",
                    )
                    db.add(session)
                    db.flush()
                    device_originated = True
                    record_session_event(
                        db,
                        device_id=device.id,
                        session_id=session.id,
                        event_type="device_start_requested",
                        event_id=request_event_id,
                        device_sequence=status.sequence,
                        device_recorded_at=status.reported_at,
                        data={"request_id": str(status.start_request_id), "lifecycle": "created"},
                    )
                status.session_id = session.id
                if device_originated and session.lifecycle == "created":
                    existing_start = db.scalar(
                        select(DeviceCommand.id).where(
                            DeviceCommand.device_id == device.id,
                            DeviceCommand.session_id == session.id,
                            DeviceCommand.command_type == DeviceCommandType.START.value,
                        ).limit(1)
                    )
                    if existing_start is None:
                        dispatch_session_id = session.id
                        dispatch_device_id = device.id

        # Publish after the session row commits. A failed/uncertain publish is
        # handled by the existing command lifecycle and never reports measuring.
        if dispatch_session_id is not None and owner_username is not None:
            try:
                self.publish_command(
                    make_command(
                        dispatch_device_id,
                        dispatch_session_id,
                        DeviceCommandType.START,
                        self.settings.mqtt_command_ttl_seconds,
                    ),
                    owner_username=owner_username,
                )
            except MessagingError:
                logger.info("device_originated_start_dispatch_failed", extra={"event": "device_originated_start_dispatch_failed"})

    def _handle_response(self, topic: DeviceTopic, body: dict[str, object]) -> None:
        if self.session_factory is None:
            raise RuntimeError("Database is unavailable.")
        try:
            response = DeviceCommandResponse.model_validate(body)
        except Exception as exc:
            raise MessageRejected("Command response schema validation failed.") from exc
        now = datetime.now(timezone.utc)
        expired_response = False
        with self.session_factory() as db:
            with db.begin():
                device = self._require_enrolled_device(db, topic, response.device_id)
                duplicate = db.scalar(
                    select(SessionEvent).where(SessionEvent.device_id == device.id, SessionEvent.event_id == response.response_id)
                )
                if duplicate is not None:
                    if duplicate.payload_sha256 and duplicate.payload_sha256 != _payload_digest(body):
                        raise MessageRejected("Command response ID was reused with different content.")
                    return
                command = db.scalar(
                    select(DeviceCommand).where(
                        DeviceCommand.command_id == response.command_id,
                        DeviceCommand.device_id == response.device_id,
                    )
                )
                if command is None:
                    raise MessageRejected("Unknown command correlation ID.")
                if command.status in {DeviceCommandStatus.TIMED_OUT.value, DeviceCommandStatus.FAILED.value} or (
                    command.status not in {DeviceCommandStatus.COMPLETED.value, DeviceCommandStatus.REJECTED.value}
                    and _as_utc(command.expires_at) <= now
                ):
                    command.status = DeviceCommandStatus.TIMED_OUT.value
                    expired_response = True
                elif command.status == response.status:
                    return  # idempotent duplicate response
                allowed = {
                    DeviceCommandStatus.CREATED.value: {"accepted", "rejected", "completed"},
                    DeviceCommandStatus.SENT.value: {"accepted", "rejected", "completed"},
                    DeviceCommandStatus.ACCEPTED.value: {"completed", "rejected"},
                }
                if not expired_response and response.status not in allowed.get(command.status, set()):
                    raise MessageRejected("Command response is not a valid state transition.")
                if expired_response:
                    measurement_session = db.get(MeasurementSession, command.session_id) if command.session_id else None
                    if measurement_session is not None and measurement_session.lifecycle in {"starting", "stopping"}:
                        measurement_session.lifecycle = "interrupted"
                        measurement_session.termination_reason = "late_command_response_after_expiry"
                    device.last_seen_at = now
                    if response.device_state is not None:
                        device.reported_state = response.device_state.value
                        device.state_reported_at = now
                    record_session_event(
                        db,
                        device_id=device.id,
                        session_id=command.session_id,
                        event_type="late_command_response",
                        event_id=response.response_id,
                        payload_sha256=_payload_digest(body),
                        device_recorded_at=response.reported_at,
                        data={"command_id": str(response.command_id), "status": response.status},
                    )
                else:
                    device.last_seen_at = now
                    if response.device_state is not None:
                        device.reported_state = response.device_state.value
                        device.state_reported_at = now
                if not expired_response and response.status == "completed":
                    if command.command_type == DeviceCommandType.START.value and response.device_state != DeviceReportedState.MEASURING:
                        raise MessageRejected("START completion requires device state measuring.")
                    if command.command_type == DeviceCommandType.STOP.value and response.device_state not in {
                        DeviceReportedState.IDLE,
                        DeviceReportedState.STOPPED,
                    }:
                        raise MessageRejected("STOP completion requires an idle or stopped device state.")
                if not expired_response:
                    command.status = response.status
                    command.response_at = now
                    command.response_code = response.reason_code
                    command.response_state = response.device_state.value if response.device_state else None
                if not expired_response and response.status == "completed":
                    command.completed_at = now
                    if command.command_type == DeviceCommandType.START.value and command.session_id:
                        measurement_session = db.get(MeasurementSession, command.session_id)
                        if measurement_session is not None:
                            measurement_session.lifecycle = "measuring"
                            measurement_session.started_at = now
                    elif command.command_type == DeviceCommandType.STOP.value and command.session_id:
                        measurement_session = db.get(MeasurementSession, command.session_id)
                        if measurement_session is not None:
                            if measurement_session.started_at is not None:
                                measurement_session.lifecycle = "completed"
                                measurement_session.ended_at = now
                            else:
                                measurement_session.lifecycle = "interrupted"
                                measurement_session.termination_reason = "start_time_unconfirmed"
                if not expired_response:
                    measurement_session = db.get(MeasurementSession, command.session_id) if command.session_id else None
                    if response.status == "accepted" and measurement_session is not None:
                        if command.command_type == DeviceCommandType.START.value and measurement_session.lifecycle == "created":
                            measurement_session.lifecycle = "starting"
                        elif command.command_type == DeviceCommandType.STOP.value and measurement_session.lifecycle in {"measuring", "interrupted"}:
                            measurement_session.lifecycle = "stopping"
                    if response.status == "rejected" and measurement_session is not None:
                        if command.command_type == DeviceCommandType.START.value:
                            measurement_session.lifecycle = "error"
                            measurement_session.termination_reason = "device_rejected_start"
                        elif command.command_type == DeviceCommandType.STOP.value:
                            if response.device_state == DeviceReportedState.MEASURING:
                                measurement_session.lifecycle = "measuring"
                            else:
                                measurement_session.lifecycle = "interrupted"
                                measurement_session.termination_reason = "stop_rejected_state_uncertain"
                    record_session_event(
                        db,
                        device_id=device.id,
                        session_id=command.session_id,
                        event_type="command_response",
                        event_id=response.response_id,
                        payload_sha256=_payload_digest(body),
                        device_recorded_at=response.reported_at,
                        data={
                            "command_id": str(response.command_id),
                            "command_type": command.command_type,
                            "status": response.status,
                            "device_state": response.device_state.value if response.device_state else None,
                            "reason_code": response.reason_code,
                        },
                    )
        if expired_response:
            raise MessageRejected("Command response arrived after expiry.")

    def publish_command(self, command: DeviceCommandRequest, *, owner_username: str | None = None) -> DeviceCommand:
        if not self._enabled or self._client is None or not self._client.is_connected():
            raise BrokerUnavailable("MQTT broker is not connected; command was not queued.")
        if owner_username is None and not self.command_authorizer.authorize(command):
            raise CommandNotAuthorized("Command was not authorized by the configured policy.")
        if self.session_factory is None:
            raise RuntimeError("Database is unavailable.")
        now = datetime.now(timezone.utc)
        if command.issued_at > now + timedelta(seconds=5):
            raise MessageRejected("Command issue time is unexpectedly in the future.")
        ttl = (command.expires_at - now).total_seconds()
        if ttl > self.settings.mqtt_command_ttl_seconds:
            raise MessageRejected("Command expiry exceeds the configured command TTL.")

        with self.session_factory() as db:
            with db.begin():
                existing = db.scalar(select(DeviceCommand).where(DeviceCommand.command_id == command.command_id))
                if existing is not None:
                    raise DuplicateCommandId("Command ID has already been used.")
                device = db.get(Device, command.device_id)
                if device is None or not device.mqtt_enabled or not device.mqtt_principal:
                    raise DeviceIdentityRejected("Device is not enrolled for MQTT messaging.")
                if owner_username is not None and device.owner_username != owner_username:
                    raise CommandNotAuthorized("Device is not owned by the authenticated API owner.")
                if not device.mqtt_principal:
                    raise DeviceIdentityRejected("Device has no MQTT topic identity.")
                with self._lock:
                    uses_legacy_topics = device.id in self._legacy_device_ids
                command_topic = (
                    legacy_device_topic(device.id, "commands")
                    if uses_legacy_topics
                    else device_topic(device.mqtt_principal, "commands")
                )
                if command.session_id is not None:
                    measurement_session = db.get(MeasurementSession, command.session_id)
                    if measurement_session is None or measurement_session.device_id != device.id:
                        raise DeviceIdentityRejected("Session does not belong to the target device.")
                    if command.command_type == DeviceCommandType.START and (
                        measurement_session.lifecycle != "created" or device.reported_state != DeviceReportedState.IDLE.value
                    ):
                        raise MessageRejected("START requires a created session and device-reported idle state.")
                    if command.command_type == DeviceCommandType.STOP and measurement_session.lifecycle not in {
                        "measuring", "stopping", "interrupted"
                    }:
                        raise MessageRejected("STOP requires an active session.")
                expired = ttl <= 0
                row = DeviceCommand(
                    command_id=command.command_id,
                    device_id=command.device_id,
                    session_id=command.session_id,
                    command_type=command.command_type.value,
                    status=DeviceCommandStatus.TIMED_OUT.value if expired else DeviceCommandStatus.CREATED.value,
                    issued_at=command.issued_at,
                    expires_at=command.expires_at,
                )
                db.add(row)
                try:
                    db.flush()
                except IntegrityError as exc:
                    raise DuplicateCommandId("Command ID has already been used.") from exc
                if expired:
                    db.flush()
                    expired_command = row
                else:
                    expired_command = None
            if expired_command is not None:
                raise CommandExpired("Command expired before publish and was not sent.")

        payload = json.dumps(command.model_dump(mode="json"), sort_keys=True, separators=(",", ":")).encode("utf-8")
        try:
            assert self._client is not None
            self._client.publish(
                command_topic,
                payload,
                expiry_seconds=max(1, int(ttl)),
                timeout_seconds=self.settings.mqtt_publish_timeout_seconds,
            )
        except Exception as exc:
            self._set_command_status(command.command_id, DeviceCommandStatus.FAILED.value, error_code="publish_failed")
            raise CommandDeliveryUncertain("MQTT command publish was attempted but its delivery is unconfirmed.") from exc

        with self.session_factory() as db:
            with db.begin():
                row = db.scalar(select(DeviceCommand).where(DeviceCommand.command_id == command.command_id))
                if row is not None and row.status == DeviceCommandStatus.CREATED.value:
                    row.status = DeviceCommandStatus.SENT.value
                    row.sent_at = datetime.now(timezone.utc)
                if row is not None and row.session_id is not None:
                    session_row = db.get(MeasurementSession, row.session_id)
                    if session_row is not None:
                        if row.command_type == DeviceCommandType.START.value and session_row.lifecycle == "created":
                            session_row.lifecycle = "starting"
                        elif row.command_type == DeviceCommandType.STOP.value and session_row.lifecycle in {"measuring", "interrupted"}:
                            session_row.lifecycle = "stopping"
                    record_session_event(
                        db,
                        device_id=row.device_id,
                        session_id=row.session_id,
                        event_type="command_sent",
                        event_id=row.command_id,
                        data={"command_id": str(row.command_id), "command_type": row.command_type, "expires_at": row.expires_at.isoformat()},
                    )
                return row

    def _set_command_status(self, command_id: UUID, status: str, *, error_code: str | None = None) -> None:
        if self.session_factory is None:
            return
        with self.session_factory() as db:
            with db.begin():
                row = db.scalar(select(DeviceCommand).where(DeviceCommand.command_id == command_id))
                if row is not None and row.status == DeviceCommandStatus.CREATED.value:
                    row.status = status
                    row.response_code = error_code
                    session_row = db.get(MeasurementSession, row.session_id) if row.session_id else None
                    if session_row is not None and session_row.lifecycle in {"created", "starting", "measuring", "stopping"}:
                        session_row.lifecycle = "interrupted"
                        session_row.termination_reason = "command_delivery_uncertain"
                        record_session_event(
                            db,
                            device_id=row.device_id,
                            session_id=session_row.id,
                            event_type="command_delivery_uncertain",
                            event_id=uuid5(row.command_id, "delivery-uncertain"),
                            data={"command_id": str(row.command_id), "command_type": row.command_type},
                        )

    def expire_commands(self) -> int:
        if self.session_factory is None:
            return 0
        now = datetime.now(timezone.utc)
        pending = [
            DeviceCommandStatus.CREATED.value,
            DeviceCommandStatus.SENT.value,
            DeviceCommandStatus.ACCEPTED.value,
        ]
        with self.session_factory() as db:
            with db.begin():
                result = db.execute(
                    update(DeviceCommand)
                    .where(DeviceCommand.status.in_(pending), DeviceCommand.expires_at <= now)
                    .values(status=DeviceCommandStatus.TIMED_OUT.value, response_code="expired")
                )
                expired_rows = db.scalars(
                    select(DeviceCommand).where(
                        DeviceCommand.status == DeviceCommandStatus.TIMED_OUT.value,
                        DeviceCommand.response_code == "expired",
                    )
                ).all()
                for command in expired_rows:
                    session_row = db.get(MeasurementSession, command.session_id) if command.session_id else None
                    if session_row is not None and session_row.lifecycle in {"starting", "stopping"}:
                        session_row.lifecycle = "interrupted"
                        session_row.termination_reason = "command_ack_timeout"
                        record_session_event(
                            db,
                            device_id=command.device_id,
                            session_id=session_row.id,
                            event_type="command_timeout",
                            event_id=uuid5(command.command_id, "timeout"),
                            data={"command_id": str(command.command_id), "command_type": command.command_type},
                        )
                return int(result.rowcount or 0)
