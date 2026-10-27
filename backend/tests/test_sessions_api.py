import json
from datetime import datetime, timedelta, timezone
from uuid import UUID, uuid4

from fastapi.testclient import TestClient
from sqlalchemy import select
from sqlalchemy.orm import sessionmaker

from app.db.session import get_db_session
from app.messaging.schemas import DeviceCommandType
from app.models import Device, DeviceCommand, MeasurementSession, Owner, OwnerCredential, SessionEvent, TelemetrySample
from app.security.auth import get_current_owner_username
from tests.test_messaging import FakeTransport, make_service, mqtt_settings
from tests.test_persistence import make_payload


def setup_owned_device(client: TestClient, db_session):
    owner = Owner(username="test_owner", display_name="test owner")
    db_session.add(owner)
    db_session.flush()
    device = Device(
        owner_username=owner.username,
        device_identifier=f"api-device-{uuid4()}",
        mqtt_enabled=True,
        reported_state="idle",
        last_seen_at=datetime.now(timezone.utc),
        state_reported_at=datetime.now(timezone.utc),
    )
    db_session.add(device)
    db_session.flush()
    device.mqtt_principal = str(device.id)
    db_session.commit()

    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    def override_db():
        with factory() as request_db:
            yield request_db
    client.app.dependency_overrides[get_db_session] = override_db
    client.app.dependency_overrides[get_current_owner_username] = lambda: owner.username
    service = make_service(db_session)[0]
    transport = service._client
    transport.connect_callback()
    client.app.state.messaging = service
    return owner, device, service, transport


def test_sessions_require_owner_bearer_authentication(client: TestClient, db_session) -> None:
    owner = Owner(username="auth_owner", display_name="auth owner")
    db_session.add(owner)
    db_session.commit()
    digest = __import__("hashlib").sha256(b"test-token").hexdigest()
    db_session.add(OwnerCredential(owner_username=owner.username, token_sha256=digest))
    db_session.commit()
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    def override_db():
        with factory() as request_db:
            yield request_db
    client.app.dependency_overrides[get_db_session] = override_db
    response = client.get("/api/v1/sessions")
    assert response.status_code == 401
    response = client.get("/api/v1/sessions", headers={"Authorization": "Bearer test-token"})
    assert response.status_code == 200


def test_create_session_is_owner_scoped_idempotent_and_waits_for_device_ack(client: TestClient, db_session) -> None:
    _owner, device, service, transport = setup_owned_device(client, db_session)
    request_key = uuid4()
    body = {"device_id": str(device.id), "idempotency_key": str(request_key)}

    created = client.post("/api/v1/sessions", json=body)
    assert created.status_code == 202
    session_id = created.json()["id"]
    assert created.json()["lifecycle"] == "starting"
    assert created.json()["latest_command"]["status"] == "sent"
    assert len(transport.publishes) == 1

    repeated = client.post("/api/v1/sessions", json=body)
    assert repeated.status_code == 200
    assert repeated.json()["id"] == session_id
    assert len(transport.publishes) == 1

    command = db_session.scalar(select(DeviceCommand).where(DeviceCommand.session_id == UUID(session_id)))
    accepted = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(command.command_id),
        "device_id": str(device.id), "status": "accepted", "device_state": "idle",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(accepted).encode(), mid=20)
    service.process_one()
    assert client.get(f"/api/v1/sessions/{session_id}").json()["lifecycle"] == "starting"

    completed = {**accepted, "response_id": str(uuid4()), "status": "completed", "device_state": "measuring"}
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(completed).encode(), mid=21)
    service.process_one()
    assert client.get(f"/api/v1/sessions/{session_id}").json()["lifecycle"] == "measuring"


def test_unowned_device_is_hidden_and_cancel_only_cancels_unstarted_session(client: TestClient, db_session) -> None:
    owner, device, _service, _transport = setup_owned_device(client, db_session)
    other = Owner(username="other_user", display_name="other")
    db_session.add(other)
    db_session.commit()
    client.app.dependency_overrides[get_current_owner_username] = lambda: other.username
    response = client.post(
        "/api/v1/sessions",
        json={"device_id": str(device.id), "idempotency_key": str(uuid4())},
    )
    assert response.status_code == 404
    assert db_session.scalar(select(MeasurementSession)) is None

    client.app.dependency_overrides[get_current_owner_username] = lambda: owner.username
    session = MeasurementSession(device_id=device.id, lifecycle="created", data_provenance="device_live")
    db_session.add(session)
    db_session.commit()
    cancelled = client.post(f"/api/v1/sessions/{session.id}/cancel")
    assert cancelled.status_code == 200
    assert cancelled.json()["lifecycle"] == "cancelled"
    assert cancelled.json()["ended_at"] is not None


def test_stop_request_requires_current_device_state_and_completion_ack(client: TestClient, db_session) -> None:
    _owner, device, service, transport = setup_owned_device(client, db_session)
    session = MeasurementSession(device_id=device.id, lifecycle="created", data_provenance="device_live")
    db_session.add(session)
    db_session.commit()
    start = service_command = __import__("app.services.session_lifecycle", fromlist=["make_command"]).make_command(
        device.id, session.id, DeviceCommandType.START, 15
    )
    service.publish_command(service_command, owner_username=device.owner_username)
    start_response = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(start.command_id),
        "device_id": str(device.id), "status": "completed", "device_state": "measuring",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(start_response).encode(), mid=30)
    service.process_one()
    assert service.health["processing_error_count"] == 0, service.health
    assert service.health["rejected_count"] == 0, service.health
    before_stop = client.get(f"/api/v1/sessions/{session.id}").json()
    assert before_stop["lifecycle"] == "measuring", before_stop
    assert before_stop["reported_device_state"] == "measuring", before_stop
    assert before_stop["device_state_fresh"] is True, before_stop

    stopping = client.post(f"/api/v1/sessions/{session.id}/complete")
    assert stopping.status_code == 202, stopping.json()
    assert stopping.json()["lifecycle"] == "stopping"
    assert stopping.json()["latest_command"]["command_type"] == "STOP"

    stop = db_session.scalar(select(DeviceCommand).where(
        DeviceCommand.session_id == session.id, DeviceCommand.command_type == "STOP"
    ))
    stop_response = {
        "schema_version": "1", "response_id": str(uuid4()), "command_id": str(stop.command_id),
        "device_id": str(device.id), "status": "completed", "device_state": "stopped",
    }
    transport.send(f"bp/v1/devices/{device.id}/responses", json.dumps(stop_response).encode(), mid=31)
    service.process_one()
    completed = client.get(f"/api/v1/sessions/{session.id}")
    assert completed.json()["lifecycle"] == "completed"


def test_samples_and_events_are_paginated_and_keep_signal_types_separate(client: TestClient, db_session) -> None:
    _owner, device, service, transport = setup_owned_device(client, db_session)
    session = MeasurementSession(device_id=device.id, lifecycle="measuring", data_provenance="device_live")
    db_session.add(session)
    db_session.commit()
    payload = make_payload(device.id, session.id, pressure=None)
    envelope = {"schema_version": "1", "payload": payload.model_dump(mode="json")}
    transport.send(f"bp/v1/devices/{device.id}/telemetry", json.dumps(envelope).encode(), mid=40)
    service.process_one()

    samples = client.get(f"/api/v1/sessions/{session.id}/samples?limit=1")
    assert samples.status_code == 200
    item = samples.json()["samples"][0]
    assert item["raw_acoustic_sample"]["adc_counts"] == 2048
    assert item["processed_signal"]["envelope"] == "1.7000"
    assert item["cuff_pressure_mmhg"] is None
    assert samples.json()["derived_measurements_available"] is False
    events = client.get(f"/api/v1/sessions/{session.id}/events?limit=1")
    assert events.json()["items"][0]["event_type"] == "telemetry_batch"


def test_sse_stream_replays_persisted_batch_notification(client: TestClient, db_session, monkeypatch) -> None:
    _owner, device, service, transport = setup_owned_device(client, db_session)
    session = MeasurementSession(device_id=device.id, lifecycle="measuring", data_provenance="device_live")
    db_session.add(session)
    db_session.commit()
    payload = make_payload(device.id, session.id, pressure=None)
    transport.send(
        f"bp/v1/devices/{device.id}/telemetry",
        json.dumps({"schema_version": "1", "payload": payload.model_dump(mode="json")}).encode(), mid=45,
    )
    service.process_one()
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    monkeypatch.setattr("app.api.routes.sessions.get_session_factory", lambda: factory)

    response = client.get(f"/api/v1/sessions/{session.id}/events/stream?max_events=1")
    assert response.status_code == 200
    assert f"event: telemetry_batch" in response.text
    assert '"samples_url"' in response.text


def test_pressure_is_rejected_without_registered_calibration(client: TestClient, db_session) -> None:
    _owner, device, service, transport = setup_owned_device(client, db_session)
    session = MeasurementSession(device_id=device.id, lifecycle="measuring", data_provenance="device_live")
    db_session.add(session)
    db_session.commit()
    payload = make_payload(device.id, session.id, pressure=120)
    envelope = {"schema_version": "1", "payload": payload.model_dump(mode="json")}
    transport.send(f"bp/v1/devices/{device.id}/telemetry", json.dumps(envelope).encode(), mid=50)
    service.process_one()
    assert service.health["rejected_count"] == 1
    assert db_session.scalar(select(TelemetrySample)) is None
