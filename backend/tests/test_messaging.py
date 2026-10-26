import json
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from uuid import uuid4

import pytest
from sqlalchemy import select
from sqlalchemy.orm import Session, sessionmaker

from app.core.config import Settings
from app.messaging.schemas import DeviceCommandRequest, DeviceCommandType
from app.messaging.service import (
    BrokerUnavailable,
    CommandExpired,
    CommandNotAuthorized,
    CommandDeliveryUncertain,
    DeviceIdentityRejected,
    DuplicateCommandId,
    MessagingService,
)
from app.models import Device, DeviceCommand, MeasurementSession, SessionEvent, TelemetrySample
from tests.test_persistence import make_payload


class FakeTransport:
    def __init__(self) -> None:
        self.on_connect = None
        self.on_disconnect = None
        self.on_message = None
        self.connected = False
        self.subscriptions = []
        self.publishes = []
        self.acks = []

    def connect_async(self) -> None:
        pass

    def loop_start(self) -> None:
        pass

    def loop_stop(self) -> None:
        pass

    def disconnect(self) -> None:
        self.connected = False

    def subscribe(self, subscriptions):
        self.subscriptions.extend(subscriptions)
        return 0, 1

    def publish(self, topic, payload, *, expiry_seconds, timeout_seconds):
        if not self.connected:
            raise ConnectionError("disconnected")
        self.publishes.append((topic, payload, expiry_seconds, timeout_seconds))

    def ack(self, mid, qos):
        self.acks.append((mid, qos))

    def is_connected(self) -> bool:
        return self.connected

    def connect_callback(self, reason_code=0):
        self.connected = reason_code == 0
        self.on_connect(self, None, {}, reason_code, None)

    def disconnect_callback(self):
        self.connected = False
        self.on_disconnect(self, None, None, 1, None)

    def send(self, topic: str, payload: bytes, mid: int = 1, qos: int = 1):
        message = SimpleNamespace(topic=topic, payload=payload, mid=mid, qos=qos)
        self.on_message(self, None, message)


class AllowCommands:
    def authorize(self, command: DeviceCommandRequest) -> bool:
        return True


def mqtt_settings() -> Settings:
    return Settings(
        _env_file=None,
        mqtt_enabled=True,
        mqtt_host="broker.example.test",
        mqtt_username="backend-service",
        mqtt_password="test-placeholder-only",
        database_url="postgresql+psycopg://placeholder:placeholder@localhost/bp_test",
    )


def registered_device(db: Session, *, reported_state: str | None = "idle") -> tuple[Device, MeasurementSession]:
    device = Device(
        device_identifier=f"mqtt-test-{uuid4()}",
        mqtt_enabled=True,
        reported_state=reported_state,
    )
    db.add(device)
    db.flush()
    device.mqtt_principal = str(device.id)
    session = MeasurementSession(device_id=device.id, lifecycle="created", data_provenance="test")
    db.add(session)
    db.commit()
    return device, session


def make_service(db_session: Session, *, authorizer=None) -> tuple[MessagingService, FakeTransport]:
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    transport = FakeTransport()
    service = MessagingService(
        mqtt_settings(), factory, transport_factory=lambda: transport, command_authorizer=authorizer
    )
    service._client = transport
    transport.on_connect = service._on_connect
    transport.on_disconnect = service._on_disconnect
    transport.on_message = service._on_message
    return service, transport


def test_connect_subscribes_only_to_device_ingress_topics(db_session: Session) -> None:
    service, transport = make_service(db_session)

    transport.connect_callback()

    assert service.health["state"] == "connected"
    assert transport.subscriptions == [
        ("bp/v1/devices/+/telemetry", 1),
        ("bp/v1/devices/+/status", 1),
        ("bp/v1/devices/+/responses", 1),
    ]
    assert not any(topic.endswith("/commands") for topic, _qos in transport.subscriptions)


def test_status_message_requires_matching_enrolled_device_identity(db_session: Session) -> None:
    device, _session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    payload = {
        "schema_version": "1",
        "message_id": str(uuid4()),
        "device_id": str(uuid4()),
        "sequence": 1,
        "state": "idle",
    }

    transport.send(f"bp/v1/devices/{device.id}/status", json.dumps(payload).encode(), mid=5)
    service.process_one()

    assert service.health["rejected_count"] == 1
    assert transport.acks == [(5, 1)]
    db_session.expire_all()
    assert db_session.get(Device, device.id).reported_state == "idle"


def test_telemetry_topic_and_payload_device_must_match(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    payload = make_payload(uuid4(), session.id)
    envelope = {"schema_version": "1", "payload": payload.model_dump(mode="json")}

    transport.send(f"bp/v1/devices/{device.id}/telemetry", json.dumps(envelope).encode(), mid=6)
    service.process_one()

    assert service.health["rejected_count"] == 1
    assert transport.acks == [(6, 1)]


def test_valid_telemetry_is_persisted_only_after_topic_identity_validation(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    payload = make_payload(device.id, session.id, pressure=None)
    envelope = {"schema_version": "1", "payload": payload.model_dump(mode="json")}

    transport.send(f"bp/v1/devices/{device.id}/telemetry", json.dumps(envelope).encode(), mid=7)
    service.process_one()

    db_session.expire_all()
    assert db_session.scalar(select(TelemetrySample)) is not None
    assert service.health["accepted_count"] == 1


def test_pressure_telemetry_requires_confirmed_device_calibration(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    payload = make_payload(device.id, session.id, pressure=120)
    envelope = {"schema_version": "1", "payload": payload.model_dump(mode="json")}
    transport.send(f"bp/v1/devices/{device.id}/telemetry", json.dumps(envelope).encode(), mid=70)
    service.process_one()
    assert db_session.scalar(select(TelemetrySample)) is None
    assert service.health["rejected_count"] == 1


def test_sequence_gaps_out_of_order_and_clock_skew_are_visible(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()

    first = make_payload(device.id, session.id, pressure=None, sample_sequence=10)
    transport.send(
        f"bp/v1/devices/{device.id}/telemetry",
        json.dumps({"schema_version": "1", "payload": first.model_dump(mode="json")}).encode(), mid=71,
    )
    service.process_one()

    gap = make_payload(device.id, session.id, pressure=None, sample_sequence=12)
    gap.samples[0].device_recorded_at = datetime.now(timezone.utc) + timedelta(hours=1)
    transport.send(
        f"bp/v1/devices/{device.id}/telemetry",
        json.dumps({"schema_version": "1", "payload": gap.model_dump(mode="json")}).encode(), mid=72,
    )
    service.process_one()
    late = make_payload(device.id, session.id, pressure=None, sample_sequence=11)
    transport.send(
        f"bp/v1/devices/{device.id}/telemetry",
        json.dumps({"schema_version": "1", "payload": late.model_dump(mode="json")}).encode(), mid=73,
    )
    service.process_one()

    db_session.expire_all()
    rows = db_session.scalars(select(TelemetrySample).order_by(TelemetrySample.sample_sequence)).all()
    assert "device_clock_skew_suspected" in rows[2].quality_flags
    assert "out_of_order_sequence" in rows[1].quality_flags
    gap_event = db_session.scalar(
        select(SessionEvent).where(SessionEvent.event_type == "telemetry_batch", SessionEvent.device_sequence == 12)
    )
    assert gap_event.data["observed_sequence_gaps"] == [{"from_sequence": 11, "to_sequence": 11}]
    assert gap_event.data["gap_is_provisional"] is True


def test_late_start_ack_marks_session_interrupted_and_never_completed(db_session: Session) -> None:
    device, session = registered_device(db_session)
    session.lifecycle = "starting"
    now = datetime.now(timezone.utc)
    command = DeviceCommand(
        command_id=uuid4(), device_id=device.id, session_id=session.id, command_type="START", status="sent",
        issued_at=now - timedelta(minutes=1), expires_at=now - timedelta(seconds=1),
    )
    db_session.add(command)
    db_session.commit()
    service, transport = make_service(db_session)
    transport.connect_callback()
    response = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(command.command_id),
        "device_id": str(device.id), "status": "completed", "device_state": "measuring",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(response).encode(), mid=74)
    service.process_one()
    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "interrupted"
    assert db_session.get(DeviceCommand, command.id).status == "timed_out"


def test_fresh_session_scoped_status_confirms_state_but_old_status_cannot_regress_it(db_session: Session) -> None:
    from app.messaging.schemas import DeviceReportedState, DeviceStatusMessage

    device, session = registered_device(db_session)
    session.lifecycle = "starting"
    db_session.commit()
    service, transport = make_service(db_session)
    transport.connect_callback()
    measuring = DeviceStatusMessage(
        schema_version="1", message_id=uuid4(), device_id=device.id, sequence=10,
        state=DeviceReportedState.MEASURING, session_id=session.id,
    )
    transport.send(
        f"bp/v1/devices/{device.id}/status", measuring.model_dump_json().encode(), mid=75
    )
    service.process_one()
    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "measuring"

    session = db_session.get(MeasurementSession, session.id)
    session.lifecycle = "stopping"
    db_session.commit()
    stopped = DeviceStatusMessage(
        schema_version="1", message_id=uuid4(), device_id=device.id, sequence=11,
        state=DeviceReportedState.STOPPED, session_id=session.id,
    )
    transport.send(f"bp/v1/devices/{device.id}/status", stopped.model_dump_json().encode(), mid=76)
    service.process_one()
    old = DeviceStatusMessage(
        schema_version="1", message_id=uuid4(), device_id=device.id, sequence=9,
        state=DeviceReportedState.MEASURING, session_id=session.id,
    )
    transport.send(f"bp/v1/devices/{device.id}/status", old.model_dump_json().encode(), mid=77)
    service.process_one()

    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "completed"
    assert db_session.get(Device, device.id).reported_state == "stopped"


def test_disconnect_updates_transport_health_and_does_not_complete_sessions(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    transport.disconnect_callback()

    assert service.health["state"] == "disconnected"
    assert service.health["disconnect_count"] == 1
    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "created"


def test_command_acknowledgements_are_correlated_before_session_completion(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    transport.connect_callback()

    def send_command(command_type: DeviceCommandType, session_id, command_id):
        issued = datetime.now(timezone.utc)
        return DeviceCommandRequest(
            command_id=command_id, device_id=device.id, session_id=session_id,
            command_type=command_type, issued_at=issued, expires_at=issued + timedelta(seconds=15),
        )

    start_command = send_command(DeviceCommandType.START, session.id, uuid4())
    service.publish_command(start_command)
    accepted = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(start_command.command_id),
        "device_id": str(device.id), "status": "accepted", "device_state": "idle",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(accepted).encode(), mid=8)
    service.process_one()

    completed_start = {**accepted, "response_id": str(uuid4()), "status": "completed", "device_state": "measuring"}
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(completed_start).encode(), mid=9)
    service.process_one()
    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "measuring"

    stop_command = send_command(DeviceCommandType.STOP, session.id, uuid4())
    service.publish_command(stop_command)
    accepted_stop = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(stop_command.command_id),
        "device_id": str(device.id), "status": "accepted", "device_state": "measuring",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(accepted_stop).encode(), mid=10)
    service.process_one()
    completed_stop = {**accepted_stop, "response_id": str(uuid4()), "status": "completed", "device_state": "stopped"}
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(completed_stop).encode(), mid=11)
    service.process_one()

    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "completed"


def test_command_authorization_fails_closed_before_publishing(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session)
    transport.connect_callback()
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id,
        command_type=DeviceCommandType.START,
        issued_at=datetime.now(timezone.utc), expires_at=datetime.now(timezone.utc) + timedelta(seconds=15),
    )

    with pytest.raises(CommandNotAuthorized):
        service.publish_command(command)

    assert transport.publishes == []
    assert db_session.scalar(select(DeviceCommand)) is None


def test_authorized_command_is_correlated_expiring_and_nonretained(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    transport.connect_callback()
    issued_at = datetime.now(timezone.utc)
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id,
        command_type=DeviceCommandType.START, issued_at=issued_at,
        expires_at=issued_at + timedelta(seconds=15),
    )

    result = service.publish_command(command)

    assert result.status == "sent"
    assert len(transport.publishes) == 1
    topic, body, expiry, _timeout = transport.publishes[0]
    assert topic == f"bp/v1/devices/{device.id}/commands"
    assert expiry <= 15
    envelope = json.loads(body)
    assert envelope["command_id"] == str(command.command_id)
    assert envelope["command_type"] == "START"


def test_duplicate_command_id_is_never_published_twice(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    transport.connect_callback()
    issued_at = datetime.now(timezone.utc)
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id,
        command_type=DeviceCommandType.START, issued_at=issued_at,
        expires_at=issued_at + timedelta(seconds=15),
    )
    service.publish_command(command)

    with pytest.raises(DuplicateCommandId):
        service.publish_command(command)

    assert len(transport.publishes) == 1


def test_expired_command_is_timed_out_without_publication(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    transport.connect_callback()
    issued_at = datetime.now(timezone.utc) - timedelta(minutes=2)
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id,
        command_type=DeviceCommandType.START, issued_at=issued_at,
        expires_at=issued_at + timedelta(seconds=10),
    )

    with pytest.raises(CommandExpired):
        service.publish_command(command)

    assert transport.publishes == []
    row = db_session.scalar(select(DeviceCommand).where(DeviceCommand.command_id == command.command_id))
    assert row.status == "timed_out"


def test_publish_while_disconnected_fails_without_queueing_command(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    issued_at = datetime.now(timezone.utc)
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id,
        command_type=DeviceCommandType.START, issued_at=issued_at,
        expires_at=issued_at + timedelta(seconds=15),
    )

    with pytest.raises(BrokerUnavailable):
        service.publish_command(command)

    assert transport.publishes == []
    assert db_session.scalar(select(DeviceCommand)) is None


def test_transport_failure_after_publish_attempt_keeps_session_uncertain(db_session: Session) -> None:
    device, session = registered_device(db_session)
    service, transport = make_service(db_session, authorizer=AllowCommands())
    transport.connect_callback()
    transport.publish = lambda *_args, **_kwargs: (_ for _ in ()).throw(TimeoutError("mock publish timeout"))
    issued = datetime.now(timezone.utc)
    command = DeviceCommandRequest(
        command_id=uuid4(), device_id=device.id, session_id=session.id, command_type=DeviceCommandType.START,
        issued_at=issued, expires_at=issued + timedelta(seconds=15),
    )
    with pytest.raises(CommandDeliveryUncertain):
        service.publish_command(command)
    db_session.expire_all()
    assert db_session.get(MeasurementSession, session.id).lifecycle == "interrupted"
    row = db_session.scalar(select(DeviceCommand).where(DeviceCommand.command_id == command.command_id))
    assert row.status == "failed"
