"""Add password hashes and expiring opaque access credentials.

Revision ID: 0006_password_auth
Revises: 0005_username_primary_key
"""

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0006_password_auth"
down_revision: Union[str, None] = "0005_username_primary_key"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.add_column("owners", sa.Column("password_hash", sa.String(length=256), nullable=True))
    op.add_column("owner_credentials", sa.Column("expires_at", sa.DateTime(timezone=True), nullable=True))
    op.create_index("ix_owner_credentials_expires_at", "owner_credentials", ["expires_at"])


def downgrade() -> None:
    op.drop_index("ix_owner_credentials_expires_at", table_name="owner_credentials")
    op.drop_column("owner_credentials", "expires_at")
    op.drop_column("owners", "password_hash")
