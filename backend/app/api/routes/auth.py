"""Password-based account registration and sign-in."""

import hashlib
import secrets
from datetime import datetime, timedelta, timezone
from functools import lru_cache
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, status
from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.db.session import get_db_session
from app.models import Owner, OwnerCredential
from app.schemas.auth import AuthResponse, CredentialsRequest, RegistrationRequest
from app.security.passwords import hash_password, verify_password

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
