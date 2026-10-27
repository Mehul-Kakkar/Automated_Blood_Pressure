"""Owner identities and revocable API credentials."""

from datetime import datetime
from uuid import UUID, uuid4

from sqlalchemy import CheckConstraint, DateTime, ForeignKey, LargeBinary, String, func
from sqlalchemy.orm import Mapped, mapped_column, relationship
from sqlalchemy.types import Uuid

from app.db.base import Base


class Owner(Base):
    __tablename__ = "owners"
    __table_args__ = (
        CheckConstraint("length(username) BETWEEN 3 AND 48 AND username = lower(username)", name="username_valid"),
    )

    username: Mapped[str] = mapped_column(String(48), primary_key=True)
    # Preserves the pre-username UUID identity for audit and migration compatibility.
    legacy_id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), nullable=False, default=uuid4)
    display_name: Mapped[str] = mapped_column(String(128), nullable=False)
    avatar_data: Mapped[bytes | None] = mapped_column(LargeBinary)
    avatar_content_type: Mapped[str | None] = mapped_column(String(32))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)

    devices: Mapped[list["Device"]] = relationship(back_populates="owner")
    credentials: Mapped[list["OwnerCredential"]] = relationship(back_populates="owner", cascade="all, delete-orphan")


class OwnerCredential(Base):
    __tablename__ = "owner_credentials"

    id: Mapped[UUID] = mapped_column(Uuid(as_uuid=True), primary_key=True, default=uuid4)
    owner_username: Mapped[str] = mapped_column(
        String(48), ForeignKey("owners.username", ondelete="CASCADE"), nullable=False, index=True
    )
    token_sha256: Mapped[str] = mapped_column(String(64), unique=True, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    revoked_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))

    owner: Mapped[Owner] = relationship(back_populates="credentials")
