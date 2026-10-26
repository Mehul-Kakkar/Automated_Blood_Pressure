"""Owner-scoped bearer credential authentication for session APIs."""

import hashlib
from typing import Annotated
from uuid import UUID

from fastapi import Depends, HTTPException, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.db.session import get_db_session
from app.models import OwnerCredential

bearer_scheme = HTTPBearer(auto_error=False, scheme_name="OwnerBearer")


def get_current_owner_id(
    credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(bearer_scheme)],
    db: Annotated[Session, Depends(get_db_session)],
) -> UUID:
    if credentials is None or credentials.scheme.lower() != "bearer":
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Bearer authentication is required.")
    if not credentials.credentials or len(credentials.credentials) > 256:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid bearer credential.")
    token_hash = hashlib.sha256(credentials.credentials.encode("utf-8")).hexdigest()
    credential = db.scalar(
        select(OwnerCredential).where(
            OwnerCredential.token_sha256 == token_hash,
            OwnerCredential.revoked_at.is_(None),
        )
    )
    if credential is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid bearer credential.")
    return credential.owner_id
