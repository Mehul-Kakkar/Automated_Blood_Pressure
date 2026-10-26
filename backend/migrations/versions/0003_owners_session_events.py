"""Add owner authorization and durable session events.

Revision ID: 0003_owners_events
Revises: 0002_mqtt_commands
"""

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0003_owners_events"
down_revision: Union[str, None] = "0002_mqtt_commands"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.create_table(
        "owners",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("display_name", sa.String(length=128), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.PrimaryKeyConstraint("id", name="pk_owners"),
    )
    op.create_table(
        "owner_credentials",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("owner_id", sa.Uuid(), nullable=False),
        sa.Column("token_sha256", sa.String(length=64), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("revoked_at", sa.DateTime(timezone=True), nullable=True),
        sa.ForeignKeyConstraint(["owner_id"], ["owners.id"], name="fk_owner_credentials_owner_id_owners", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_owner_credentials"),
        sa.UniqueConstraint("token_sha256", name="uq_owner_credentials_token_sha256"),
    )
    op.create_index("ix_owner_credentials_owner_id", "owner_credentials", ["owner_id"])
    with op.batch_alter_table("devices") as batch_op:
        batch_op.add_column(sa.Column("owner_id", sa.Uuid(), nullable=True))
        batch_op.add_column(sa.Column("pressure_calibration_confirmed", sa.Boolean(), server_default=sa.text("false"), nullable=False))
        batch_op.add_column(sa.Column("pressure_calibration_version", sa.String(length=64), nullable=True))
        batch_op.add_column(sa.Column("state_reported_at", sa.DateTime(timezone=True), nullable=True))
        batch_op.create_check_constraint(
            "ck_devices_pressure_calibration_requires_version",
            "pressure_calibration_confirmed = false OR pressure_calibration_version IS NOT NULL",
        )
        batch_op.create_foreign_key("fk_devices_owner_id_owners", "owners", ["owner_id"], ["id"], ondelete="SET NULL")
    op.create_index("ix_devices_owner_id", "devices", ["owner_id"])
    with op.batch_alter_table("measurement_sessions") as batch_op:
        batch_op.add_column(sa.Column("client_request_id", sa.Uuid(), nullable=True))
        batch_op.create_unique_constraint(
            "uq_measurement_sessions_device_request", ["device_id", "client_request_id"]
        )
    op.create_table(
        "session_events",
        sa.Column("id", sa.Integer(), autoincrement=True, nullable=False),
        sa.Column("event_id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=True),
        sa.Column("event_type", sa.String(length=48), nullable=False),
        sa.Column("payload_sha256", sa.String(length=64), nullable=True),
        sa.Column("device_sequence", sa.BigInteger(), nullable=True),
        sa.Column("device_recorded_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("received_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("data", sa.JSON(), nullable=False),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_session_events_device_id_devices", ondelete="RESTRICT"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_session_events_session_id_measurement_sessions", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_session_events"),
        sa.UniqueConstraint("device_id", "event_id", name="uq_session_events_device_event"),
    )
    op.create_index("ix_session_events_session_id_id", "session_events", ["session_id", "id"])


def downgrade() -> None:
    op.drop_index("ix_session_events_session_id_id", table_name="session_events")
    op.drop_table("session_events")
    with op.batch_alter_table("measurement_sessions") as batch_op:
        batch_op.drop_constraint("uq_measurement_sessions_device_request", type_="unique")
        batch_op.drop_column("client_request_id")
    op.drop_index("ix_devices_owner_id", table_name="devices")
    with op.batch_alter_table("devices") as batch_op:
        batch_op.drop_constraint("fk_devices_owner_id_owners", type_="foreignkey")
        batch_op.drop_constraint("ck_devices_pressure_calibration_requires_version", type_="check")
        batch_op.drop_column("pressure_calibration_version")
        batch_op.drop_column("pressure_calibration_confirmed")
        batch_op.drop_column("state_reported_at")
        batch_op.drop_column("owner_id")
    op.drop_index("ix_owner_credentials_owner_id", table_name="owner_credentials")
    op.drop_table("owner_credentials")
    op.drop_table("owners")
