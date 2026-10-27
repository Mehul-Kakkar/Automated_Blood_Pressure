from datetime import datetime, timedelta, timezone

import pytest
from fastapi import HTTPException
from sqlalchemy import select

from app.api.routes.auth import login, register
from app.models import Owner, OwnerCredential
from app.schemas.auth import CredentialsRequest, RegistrationRequest
from app.security.auth import get_current_owner_username
from app.security.passwords import hash_password, verify_password
from fastapi.security import HTTPAuthorizationCredentials


def test_password_hash_is_salted_and_verifiable() -> None:
    first = hash_password("correct horse battery staple")
    second = hash_password("correct horse battery staple")
    assert first != second
    assert verify_password("correct horse battery staple", first)
    assert not verify_password("wrong password", first)
    assert not verify_password("correct horse battery staple", "malformed-hash")


def test_registration_and_login_create_only_hashed_credentials(db_session) -> None:
    registered = register(RegistrationRequest(
        username="Secure_User", display_name="Display Name", password="correct horse battery staple"
    ), db_session)
    owner = db_session.get(Owner, "secure_user")
    assert owner is not None
    assert owner.display_name == "Display Name"
    assert owner.password_hash != "correct horse battery staple"
    assert verify_password("correct horse battery staple", owner.password_hash or "")
    stored = db_session.scalar(select(OwnerCredential).where(OwnerCredential.owner_username == owner.username))
    assert stored is not None
    assert stored.token_sha256 != registered.access_token
    assert stored.expires_at is not None

    logged_in = login(CredentialsRequest(username="SECURE_USER", password="correct horse battery staple"), db_session)
    assert logged_in.username == "secure_user"
    assert logged_in.access_token != registered.access_token
    assert db_session.scalar(select(OwnerCredential).where(
        OwnerCredential.token_sha256 == __import__("hashlib").sha256(logged_in.access_token.encode()).hexdigest()
    )) is not None


def test_login_rejects_wrong_password_and_legacy_owner_without_password(db_session) -> None:
    db_session.add(Owner(username="legacy_user", display_name="Legacy", password_hash=None))
    db_session.commit()
    with pytest.raises(HTTPException) as wrong:
        login(CredentialsRequest(username="legacy_user", password="incorrect password"), db_session)
    assert wrong.value.status_code == 401
    with pytest.raises(HTTPException) as absent:
        login(CredentialsRequest(username="missing_user", password="incorrect password"), db_session)
    assert absent.value.status_code == 401


def test_expired_and_valid_credentials_are_handled(db_session) -> None:
    owner = Owner(username="expiry_user", display_name="Expiry", password_hash=hash_password("correct horse battery staple"))
    db_session.add(owner)
    db_session.flush()
    expired = OwnerCredential(owner_username=owner.username, token_sha256="1" * 64,
                              expires_at=datetime.now(timezone.utc) - timedelta(seconds=1))
    valid = OwnerCredential(owner_username=owner.username, token_sha256="2" * 64,
                            expires_at=datetime.now(timezone.utc) + timedelta(minutes=5))
    db_session.add_all([expired, valid])
    db_session.commit()
    with pytest.raises(HTTPException) as expired_error:
        get_current_owner_username(HTTPAuthorizationCredentials(scheme="Bearer", credentials="expired-token"), db_session)
    assert expired_error.value.status_code == 401
    assert get_current_owner_username(HTTPAuthorizationCredentials(scheme="Bearer", credentials="valid-token"), db_session) == owner.username
