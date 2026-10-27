"""Owner-scoped persistent profile operations."""

import base64
import binascii
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, Response
from sqlalchemy.orm import Session

from app.db.session import get_db_session
from app.models import Owner
from app.schemas.profile import AvatarUpdateRequest, ProfileResponse, ProfileUpdateRequest
from app.security.auth import get_current_owner_username

router = APIRouter(prefix="/api/v1/profile", tags=["profile"])
Db = Annotated[Session, Depends(get_db_session)]
OwnerUsername = Annotated[str, Depends(get_current_owner_username)]
MAX_AVATAR_BYTES = 1_048_576
MAGIC: dict[str, tuple[bytes, ...]] = {
    "image/png": (b"\x89PNG\r\n\x1a\n",),
    "image/jpeg": (b"\xff\xd8\xff",),
    "image/gif": (b"GIF87a", b"GIF89a"),
    "image/webp": (b"RIFF",),
}


def _owner(db: Session, username: str) -> Owner:
    owner = db.get(Owner, username)
    if owner is None:
        raise HTTPException(status_code=401, detail="Authenticated owner no longer exists.")
    return owner


def _response(owner: Owner) -> ProfileResponse:
    return ProfileResponse(username=owner.username, display_name=owner.display_name,
                           created_at=owner.created_at, avatar_available=owner.avatar_data is not None)


@router.get("", response_model=ProfileResponse)
def get_profile(db: Db, username: OwnerUsername) -> ProfileResponse:
    return _response(_owner(db, username))


@router.put("", response_model=ProfileResponse)
def update_profile(body: ProfileUpdateRequest, db: Db, username: OwnerUsername) -> ProfileResponse:
    owner = _owner(db, username)
    owner.display_name = body.display_name
    db.commit()
    db.refresh(owner)
    return _response(owner)


@router.put("/avatar", response_model=ProfileResponse)
def update_avatar(body: AvatarUpdateRequest, db: Db, username: OwnerUsername) -> ProfileResponse:
    try:
        data = base64.b64decode(body.data_base64, validate=True)
    except (ValueError, binascii.Error) as exc:
        raise HTTPException(status_code=422, detail="Avatar data must be valid base64.") from exc
    if not data or len(data) > MAX_AVATAR_BYTES:
        raise HTTPException(status_code=422, detail="Avatar must be no larger than 1 MiB.")
    if not any(data.startswith(prefix) for prefix in MAGIC[body.content_type]):
        raise HTTPException(status_code=422, detail="Avatar content does not match its declared image type.")
    # RIFF-based WebP also includes WEBP at byte 8.
    if body.content_type == "image/webp" and data[8:12] != b"WEBP":
        raise HTTPException(status_code=422, detail="Avatar content does not match its declared image type.")
    owner = _owner(db, username)
    owner.avatar_data = data
    owner.avatar_content_type = body.content_type
    db.commit()
    db.refresh(owner)
    return _response(owner)


@router.get("/avatar")
def get_avatar(db: Db, username: OwnerUsername) -> Response:
    owner = _owner(db, username)
    if owner.avatar_data is None or owner.avatar_content_type is None:
        raise HTTPException(status_code=404, detail="Profile avatar is not set.")
    return Response(content=owner.avatar_data, media_type=owner.avatar_content_type,
                    headers={"Cache-Control": "private, no-store", "X-Content-Type-Options": "nosniff"})


@router.delete("/avatar", response_model=ProfileResponse)
def delete_avatar(db: Db, username: OwnerUsername) -> ProfileResponse:
    owner = _owner(db, username)
    owner.avatar_data = None
    owner.avatar_content_type = None
    db.commit()
    db.refresh(owner)
    return _response(owner)
