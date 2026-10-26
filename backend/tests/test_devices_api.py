import pytest
from fastapi.testclient import TestClient
from sqlalchemy.orm import sessionmaker
from app.db.session import get_db_session
from app.models import Owner
from app.security.auth import get_current_owner_id


def test_owner_can_register_list_and_read_device_but_registration_is_not_mqtt_enrollment(client: TestClient, db_session) -> None:
    owner = Owner(display_name="device api owner")
    db_session.add(owner)
    db_session.commit()
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)
    def override_db():
        with factory() as request_db:
            yield request_db
    client.app.dependency_overrides[get_db_session] = override_db
    client.app.dependency_overrides[get_current_owner_id] = lambda: owner.id
    registered = client.post("/api/v1/devices", json={"device_identifier": "synthetic-device", "hardware_revision": "test"})
    assert registered.status_code == 201
    device = registered.json()
    assert device["mqtt_enrolled"] is False
    assert device["online"] is False
    assert client.get("/api/v1/devices").json()["items"][0]["id"] == device["id"]
    assert client.get(f"/api/v1/devices/{device['id']}").status_code == 200
    client.app.dependency_overrides[get_current_owner_id] = lambda: __import__("uuid").uuid4()
    assert client.get(f"/api/v1/devices/{device['id']}").status_code == 404
    duplicate = client.post("/api/v1/devices", json={"device_identifier": "synthetic-device"})
    assert duplicate.status_code == 409
