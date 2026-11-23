"""Password-based account registration and sign-in."""

import hashlib
import secrets
from datetime import datetime, timedelta, timezone
from functools import lru_cache
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.db.session import get_db_session
from app.models import Owner, OwnerCredential
from app.schemas.auth import AuthResponse, CredentialsRequest, RegistrationRequest
from app.security.passwords import hash_password, verify_password
from app.security.auth import bearer_scheme

router = APIRouter(prefix="/api/v1/auth", tags=["authentication"])
Db = Annotated[Session, Depends(get_db_session)]
ACCESS_TOKEN_LIFETIME = timedelta(days=30)


@lru_cache(maxsize=1)
def _dummy_password_hash() -> str:
    return hash_password(secrets.token_urlsafe(32))


def _issue_credential(db: Session, owner: Owner) -> AuthResponse:
    token = secrets.token_urlsafe(32)
    expires_at = datetime.now(timezone.utc) + ACCESS_TOKEN_LIFETIME
    credential = OwnerCredential(
        owner_username=owner.username,
        token_sha256=hashlib.sha256(token.encode("utf-8")).hexdigest(),
        expires_at=expires_at,
    )
    db.add(credential)
    return AuthResponse(access_token=token, expires_at=expires_at.isoformat(),
                        username=owner.username, display_name=owner.display_name)


@router.post("/register", response_model=AuthResponse, status_code=status.HTTP_201_CREATED)
def register(body: RegistrationRequest, db: Db) -> AuthResponse:
    if db.get(Owner, body.username) is not None:
        raise HTTPException(status_code=409, detail="Username is already registered.")
    owner = Owner(username=body.username, display_name=body.display_name, password_hash=hash_password(body.password))
    db.add(owner)
    result = _issue_credential(db, owner)
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise HTTPException(status_code=409, detail="Username is already registered.") from exc
    return result


@router.post("/login", response_model=AuthResponse)
def login(body: CredentialsRequest, db: Db) -> AuthResponse:
    owner = db.scalar(select(Owner).where(Owner.username == body.username))
    stored_hash = owner.password_hash if owner and owner.password_hash else _dummy_password_hash()
    password_matches = verify_password(body.password, stored_hash)
    if owner is None or owner.password_hash is None or not password_matches:
        raise HTTPException(status_code=401, detail="Username or password is incorrect.",
                            headers={"WWW-Authenticate": "Bearer"})
    result = _issue_credential(db, owner)
    db.commit()
    return result


def _active_credential(db: Session, credentials: HTTPAuthorizationCredentials | None) -> OwnerCredential:
    if credentials is None or credentials.scheme.lower() != "bearer" or not credentials.credentials:
        raise HTTPException(status_code=401, detail="Bearer authentication is required.")
    digest = hashlib.sha256(credentials.credentials.encode("utf-8")).hexdigest()
    credential = db.scalar(select(OwnerCredential).where(
        OwnerCredential.token_sha256 == digest,
        OwnerCredential.revoked_at.is_(None),
        (OwnerCredential.expires_at.is_(None) | (OwnerCredential.expires_at > datetime.now(timezone.utc))),
    ))
    if credential is None:
        raise HTTPException(status_code=401, detail="Invalid bearer credential.")
    return credential


@router.post("/refresh", response_model=AuthResponse)
def refresh(credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)], db: Db) -> AuthResponse:
    """Rotate a still-valid bearer token; the old token becomes unusable."""
    current = _active_credential(db, credentials)
    owner = db.get(Owner, current.owner_username)
    if owner is None:
        raise HTTPException(status_code=401, detail="Account no longer exists.")
    current.revoked_at = datetime.now(timezone.utc)
    result = _issue_credential(db, owner)
    db.commit()
    return result


@router.post("/logout", status_code=status.HTTP_204_NO_CONTENT)
def logout(credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)], db: Db) -> None:
    """Revoke the current server-side session credential."""
    current = _active_credential(db, credentials)
    current.revoked_at = datetime.now(timezone.utc)
    db.commit()
