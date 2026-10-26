"""Add MQTT enrollment identity and expiring command audit state.

Revision ID: 0002_mqtt_commands
Revises: 0001_initial
"""

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0002_mqtt_commands"
down_revision: Union[str, None] = "0001_initial"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    with op.batch_alter_table("devices") as batch_op:
        batch_op.add_column(sa.Column("mqtt_enabled", sa.Boolean(), server_default=sa.text("false"), nullable=False))
        batch_op.add_column(sa.Column("mqtt_principal", sa.String(length=128), nullable=True))
        batch_op.add_column(sa.Column("reported_state", sa.String(length=24), nullable=True))
        batch_op.add_column(sa.Column("reported_lost_sample_count", sa.Integer(), server_default=sa.text("0"), nullable=False))
        batch_op.create_unique_constraint("uq_devices_mqtt_principal", ["mqtt_principal"])

    op.create_table(
        "device_commands",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("command_id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=True),
        sa.Column("command_type", sa.String(length=24), nullable=False),
        sa.Column("status", sa.String(length=16), nullable=False),
        sa.Column("issued_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("sent_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("response_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("response_code", sa.String(length=64), nullable=True),
        sa.Column("response_state", sa.String(length=24), nullable=True),
        sa.CheckConstraint("status IN ('created', 'sent', 'accepted', 'rejected', 'completed', 'timed_out', 'failed')", name="ck_device_commands_status_valid"),
        sa.CheckConstraint("command_type IN ('START', 'STOP', 'STATUS_REQUEST')", name="ck_device_commands_command_type_valid"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_device_commands_device_id_devices", ondelete="RESTRICT"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_device_commands_session_id_measurement_sessions", ondelete="SET NULL"),
        sa.PrimaryKeyConstraint("id", name="pk_device_commands"),
        sa.UniqueConstraint("command_id", name="uq_device_commands_command_id"),
    )
    op.create_index("ix_device_commands_device_status", "device_commands", ["device_id", "status"])
    op.create_index("ix_device_commands_expires_status", "device_commands", ["expires_at", "status"])


def downgrade() -> None:
    op.drop_index("ix_device_commands_expires_status", table_name="device_commands")
    op.drop_index("ix_device_commands_device_status", table_name="device_commands")
    op.drop_table("device_commands")
    with op.batch_alter_table("devices") as batch_op:
        batch_op.drop_constraint("uq_devices_mqtt_principal", type_="unique")
        batch_op.drop_column("reported_state")
        batch_op.drop_column("reported_lost_sample_count")
        batch_op.drop_column("mqtt_principal")
        batch_op.drop_column("mqtt_enabled")
