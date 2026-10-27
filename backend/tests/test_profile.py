import base64
from uuid import uuid4

from fastapi.testclient import TestClient
import pytest
from sqlalchemy import insert
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import sessionmaker

from app.db.session import get_db_session
from app.models import Owner
from app.security.auth import get_current_owner_username


def test_profile_persists_username_display_name_and_avatar(client: TestClient, db_session) -> None:
    owner = Owner(username=f"profile_{uuid4().hex[:8]}", display_name="Initial Name")
    db_session.add(owner)
    db_session.commit()
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)

    def override_db():
        with factory() as request_db:
            yield request_db

    client.app.dependency_overrides[get_db_session] = override_db
    client.app.dependency_overrides[get_current_owner_username] = lambda: owner.username

    profile = client.get("/api/v1/profile")
    assert profile.status_code == 200
    assert profile.json()["username"] == owner.username
    assert profile.json()["display_name"] == "Initial Name"

    updated = client.put("/api/v1/profile", json={"display_name": "New Display Name"})
    assert updated.status_code == 200
    assert updated.json()["username"] == owner.username
    assert updated.json()["display_name"] == "New Display Name"

    # Minimal valid 1x1 PNG header; the endpoint stores supplied bytes and labels,
    # it does not re-encode or interpret the image.
    png = b"\x89PNG\r\n\x1a\n" + b"synthetic-test"
    uploaded = client.put("/api/v1/profile/avatar", json={
        "content_type": "image/png", "data_base64": base64.b64encode(png).decode("ascii")
    })
    assert uploaded.status_code == 200
    assert uploaded.json()["avatar_available"] is True
    image = client.get("/api/v1/profile/avatar")
    assert image.status_code == 200
    assert image.content == png
    assert image.headers["cache-control"] == "private, no-store"

    removed = client.delete("/api/v1/profile/avatar")
    assert removed.status_code == 200
    assert removed.json()["avatar_available"] is False
    client.app.dependency_overrides.clear()


def test_profile_rejects_blank_names_and_invalid_avatar_content(client: TestClient, db_session) -> None:
    owner = Owner(username=f"profile_{uuid4().hex[:8]}", display_name="Owner")
    db_session.add(owner)
    db_session.commit()
    factory = sessionmaker(bind=db_session.get_bind(), expire_on_commit=False)

    def override_db():
        with factory() as request_db:
            yield request_db

    client.app.dependency_overrides[get_db_session] = override_db
    client.app.dependency_overrides[get_current_owner_username] = lambda: owner.username
    assert client.put("/api/v1/profile", json={"display_name": "  "}).status_code == 422
    malformed = client.put("/api/v1/profile/avatar", json={
        "content_type": "image/png", "data_base64": base64.b64encode(b"not an image").decode("ascii")
    })
    assert malformed.status_code == 422
    client.app.dependency_overrides.clear()


def test_username_primary_key_is_unique_and_separate_from_display_name(db_session) -> None:
    first = Owner(username="same_user", display_name="First Name")
    db_session.add(first)
    db_session.commit()
    with pytest.raises(IntegrityError):
        db_session.execute(insert(Owner).values(username="same_user", display_name="Different Name", legacy_id=uuid4()))
        db_session.commit()
    db_session.rollback()
