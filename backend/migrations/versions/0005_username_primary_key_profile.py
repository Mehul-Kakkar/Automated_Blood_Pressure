"""Use username as the owner primary key and persist profile fields.

Revision ID: 0005_username_primary_key
Revises: 0004_report_snapshot
"""

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0005_username_primary_key"
down_revision: Union[str, None] = "0004_report_snapshot"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    # Preserve each existing UUID as a non-key legacy identifier. Existing records
    # receive deterministic usernames so their credentials and device ownership survive.
    op.add_column("owners", sa.Column("username", sa.String(length=48), nullable=True))
    bind = op.get_bind()
    rows = bind.execute(sa.text("SELECT id FROM owners")).all()
    for (owner_id,) in rows:
        compact = str(owner_id).replace("-", "").lower()
        bind.execute(
            sa.text("UPDATE owners SET username = :username WHERE id = :owner_id"),
            {"username": f"user_{compact}", "owner_id": owner_id},
        )

    op.add_column("owner_credentials", sa.Column("owner_username", sa.String(length=48), nullable=True))
    op.add_column("devices", sa.Column("owner_username", sa.String(length=48), nullable=True))
    bind.execute(sa.text(
        "UPDATE owner_credentials SET owner_username = "
        "(SELECT username FROM owners WHERE owners.id = owner_credentials.owner_id)"
    ))
    bind.execute(sa.text(
        "UPDATE devices SET owner_username = "
        "(SELECT username FROM owners WHERE owners.id = devices.owner_id)"
    ))

    # Drop the old references before changing the owners primary key (SQLite requires
    # table recreation for these changes).
    with op.batch_alter_table("owner_credentials") as batch:
        batch.drop_constraint("fk_owner_credentials_owner_id_owners", type_="foreignkey")
    with op.batch_alter_table("devices") as batch:
        batch.drop_constraint("fk_devices_owner_id_owners", type_="foreignkey")
    op.drop_index("ix_owner_credentials_owner_id", table_name="owner_credentials")
    op.drop_index("ix_devices_owner_id", table_name="devices")
    with op.batch_alter_table("owner_credentials") as batch:
        batch.drop_column("owner_id")
        batch.alter_column("owner_username", existing_type=sa.String(48), nullable=False)
    with op.batch_alter_table("devices") as batch:
        batch.drop_column("owner_id")

    with op.batch_alter_table("owners") as batch:
        batch.drop_constraint("pk_owners", type_="primary")
        batch.alter_column("id", new_column_name="legacy_id", existing_type=sa.Uuid(), nullable=False)
        batch.alter_column("username", existing_type=sa.String(48), nullable=False)
        batch.add_column(sa.Column("avatar_data", sa.LargeBinary(), nullable=True))
        batch.add_column(sa.Column("avatar_content_type", sa.String(length=32), nullable=True))
        batch.create_primary_key("pk_owners", ["username"])
        batch.create_check_constraint(op.f("ck_owners_username_valid"), "length(username) BETWEEN 3 AND 48 AND username = lower(username)")

    with op.batch_alter_table("owner_credentials") as batch:
        batch.create_foreign_key(
            "fk_owner_credentials_owner_username_owners", "owners", ["owner_username"], ["username"], ondelete="CASCADE"
        )
    with op.batch_alter_table("devices") as batch:
        batch.create_foreign_key(
            "fk_devices_owner_username_owners", "owners", ["owner_username"], ["username"], ondelete="SET NULL"
        )
    op.create_index("ix_owner_credentials_owner_username", "owner_credentials", ["owner_username"])
    op.create_index("ix_devices_owner_username", "devices", ["owner_username"])


def downgrade() -> None:
    # Restore the original UUID primary key while keeping usernames as unique data.
    with op.batch_alter_table("owner_credentials") as batch:
        batch.drop_constraint("fk_owner_credentials_owner_username_owners", type_="foreignkey")
    with op.batch_alter_table("devices") as batch:
        batch.drop_constraint("fk_devices_owner_username_owners", type_="foreignkey")
    op.drop_index("ix_owner_credentials_owner_username", table_name="owner_credentials")
    op.drop_index("ix_devices_owner_username", table_name="devices")
    with op.batch_alter_table("owner_credentials") as batch:
        batch.add_column(sa.Column("owner_id", sa.Uuid(), nullable=True))
    with op.batch_alter_table("devices") as batch:
        batch.add_column(sa.Column("owner_id", sa.Uuid(), nullable=True))
    bind = op.get_bind()
    bind.execute(sa.text(
        "UPDATE owner_credentials SET owner_id = "
        "(SELECT legacy_id FROM owners WHERE owners.username = owner_credentials.owner_username)"
    ))
    bind.execute(sa.text(
        "UPDATE devices SET owner_id = "
        "(SELECT legacy_id FROM owners WHERE owners.username = devices.owner_username)"
    ))
    with op.batch_alter_table("owners") as batch:
        batch.drop_constraint(op.f("ck_owners_username_valid"), type_="check")
        batch.drop_constraint("pk_owners", type_="primary")
        batch.drop_column("avatar_content_type")
        batch.drop_column("avatar_data")
        batch.alter_column("legacy_id", new_column_name="id", existing_type=sa.Uuid(), nullable=False)
        batch.drop_column("username")
        batch.create_primary_key("pk_owners", ["id"])
    with op.batch_alter_table("owner_credentials") as batch:
        batch.drop_column("owner_username")
        batch.alter_column("owner_id", existing_type=sa.Uuid(), nullable=False)
        batch.create_foreign_key("fk_owner_credentials_owner_id_owners", "owners", ["owner_id"], ["id"], ondelete="CASCADE")
    with op.batch_alter_table("devices") as batch:
        batch.drop_column("owner_username")
        batch.create_foreign_key("fk_devices_owner_id_owners", "owners", ["owner_id"], ["id"], ondelete="SET NULL")
    op.create_index("ix_owner_credentials_owner_id", "owner_credentials", ["owner_id"])
    op.create_index("ix_devices_owner_id", "devices", ["owner_id"])
