"""Create the initial device measurement persistence schema.

Revision ID: 0001_initial
Revises:
"""

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0001_initial"
down_revision: Union[str, None] = None
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.create_table(
        "devices",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("device_identifier", sa.String(length=128), nullable=False),
        sa.Column("hardware_revision", sa.String(length=64), nullable=True),
        sa.Column("firmware_version", sa.String(length=64), nullable=True),
        sa.Column("processing_algorithm_version", sa.String(length=64), nullable=True),
        sa.Column("registered_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("last_seen_at", sa.DateTime(timezone=True), nullable=True),
        sa.PrimaryKeyConstraint("id", name="pk_devices"),
        sa.UniqueConstraint("device_identifier", name="uq_devices_device_identifier"),
    )
    op.create_table(
        "measurement_sessions",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("lifecycle", sa.String(length=24), nullable=False),
        sa.Column("protocol_version", sa.String(length=32), nullable=True),
        sa.Column("firmware_version", sa.String(length=64), nullable=True),
        sa.Column("processing_algorithm_version", sa.String(length=64), nullable=True),
        sa.Column("data_provenance", sa.String(length=32), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("started_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("ended_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("termination_reason", sa.String(length=128), nullable=True),
        sa.CheckConstraint(
            "lifecycle IN ('created', 'starting', 'measuring', 'stopping', 'completed', 'cancelled', 'interrupted', 'error')",
            name="ck_measurement_sessions_lifecycle_valid",
        ),
        sa.CheckConstraint("ended_at IS NULL OR started_at IS NULL OR ended_at >= started_at", name="ck_measurement_sessions_time_order"),
        sa.CheckConstraint("lifecycle != 'completed' OR (started_at IS NOT NULL AND ended_at IS NOT NULL)", name="ck_measurement_sessions_completed_requires_confirmed_times"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_measurement_sessions_device_id_devices", ondelete="RESTRICT"),
        sa.PrimaryKeyConstraint("id", name="pk_measurement_sessions"),
    )
    op.create_index("ix_measurement_sessions_device_created", "measurement_sessions", ["device_id", "created_at"])
    op.create_table(
        "telemetry_batches",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("batch_id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=False),
        sa.Column("payload_sha256", sa.String(length=64), nullable=False),
        sa.Column("sample_count", sa.Integer(), nullable=False),
        sa.Column("event_count", sa.Integer(), nullable=False),
        sa.Column("schema_version", sa.String(length=32), nullable=False),
        sa.Column("firmware_version", sa.String(length=64), nullable=True),
        sa.Column("processing_algorithm_version", sa.String(length=64), nullable=True),
        sa.Column("data_provenance", sa.String(length=32), nullable=False),
        sa.Column("sync_status", sa.String(length=24), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.CheckConstraint("sample_count BETWEEN 1 AND 500", name="ck_telemetry_batches_sample_count_bounded"),
        sa.CheckConstraint("event_count BETWEEN 0 AND 500", name="ck_telemetry_batches_event_count_bounded"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_telemetry_batches_device_id_devices", ondelete="RESTRICT"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_telemetry_batches_session_id_measurement_sessions", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_telemetry_batches"),
        sa.UniqueConstraint("device_id", "batch_id", name="uq_telemetry_batches_device_id_batch_id"),
    )
    op.create_index("ix_telemetry_batches_session_received", "telemetry_batches", ["session_id", "received_at"])
    op.create_table(
        "telemetry_samples",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("sample_id", sa.Uuid(), nullable=False),
        sa.Column("batch_row_id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=False),
        sa.Column("sample_sequence", sa.BigInteger(), nullable=True),
        sa.Column("device_timestamp_ms", sa.BigInteger(), nullable=True),
        sa.Column("device_recorded_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("received_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("pressure_mmhg", sa.Numeric(precision=7, scale=2), nullable=True),
        sa.Column("pressure_unit", sa.String(length=8), nullable=True),
        sa.Column("heart_rate_bpm", sa.Integer(), nullable=True),
        sa.Column("raw_adc", sa.Integer(), nullable=True),
        sa.Column("filtered", sa.Numeric(precision=12, scale=4), nullable=True),
        sa.Column("acoustic_envelope", sa.Numeric(precision=12, scale=4), nullable=True),
        sa.Column("noise_floor", sa.Numeric(precision=12, scale=4), nullable=True),
        sa.Column("threshold", sa.Numeric(precision=12, scale=4), nullable=True),
        sa.Column("event_detected", sa.Boolean(), nullable=True),
        sa.Column("is_valid", sa.Boolean(), nullable=False),
        sa.Column("quality_flags", sa.JSON(), nullable=False),
        sa.Column("firmware_version", sa.String(length=64), nullable=True),
        sa.Column("processing_algorithm_version", sa.String(length=64), nullable=True),
        sa.Column("data_provenance", sa.String(length=32), nullable=False),
        sa.Column("synchronization_metadata", sa.JSON(), nullable=False),
        sa.CheckConstraint("sample_sequence IS NULL OR sample_sequence >= 0", name="ck_telemetry_samples_sample_sequence_nonnegative"),
        sa.CheckConstraint("device_timestamp_ms IS NULL OR device_timestamp_ms >= 0", name="ck_telemetry_samples_device_time_nonnegative"),
        sa.CheckConstraint("pressure_mmhg IS NULL OR pressure_mmhg BETWEEN 0 AND 400", name="ck_telemetry_samples_pressure_range"),
        sa.CheckConstraint("(pressure_mmhg IS NULL AND pressure_unit IS NULL) OR (pressure_mmhg IS NOT NULL AND pressure_unit IS NOT NULL AND pressure_unit = 'mmHg')", name="ck_telemetry_samples_pressure_unit_explicit"),
        sa.CheckConstraint("heart_rate_bpm IS NULL OR heart_rate_bpm BETWEEN 25 AND 240", name="ck_telemetry_samples_heart_rate_range"),
        sa.CheckConstraint("raw_adc IS NULL OR raw_adc BETWEEN 0 AND 4095", name="ck_telemetry_samples_raw_adc_range"),
        sa.CheckConstraint("acoustic_envelope IS NULL OR acoustic_envelope >= 0", name="ck_telemetry_samples_envelope_nonnegative"),
        sa.CheckConstraint("noise_floor IS NULL OR noise_floor >= 0", name="ck_telemetry_samples_noise_floor_nonnegative"),
        sa.CheckConstraint("threshold IS NULL OR threshold >= 0", name="ck_telemetry_samples_threshold_nonnegative"),
        sa.ForeignKeyConstraint(["batch_row_id"], ["telemetry_batches.id"], name="fk_telemetry_samples_batch_row_id_telemetry_batches", ondelete="CASCADE"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_telemetry_samples_device_id_devices", ondelete="RESTRICT"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_telemetry_samples_session_id_measurement_sessions", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_telemetry_samples"),
        sa.UniqueConstraint("device_id", "sample_id", name="uq_telemetry_samples_device_id_sample_id"),
        sa.UniqueConstraint("session_id", "sample_sequence", name="uq_telemetry_samples_session_id_sample_sequence"),
    )
    op.create_index("ix_telemetry_samples_session_received", "telemetry_samples", ["session_id", "received_at"])
    op.create_table(
        "acoustic_events",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("event_id", sa.Uuid(), nullable=False),
        sa.Column("batch_row_id", sa.Uuid(), nullable=False),
        sa.Column("device_id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=False),
        sa.Column("event_sequence", sa.BigInteger(), nullable=True),
        sa.Column("event_type", sa.String(length=48), nullable=False),
        sa.Column("device_timestamp_ms", sa.BigInteger(), nullable=True),
        sa.Column("device_recorded_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("received_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("strength", sa.Numeric(precision=12, scale=4), nullable=True),
        sa.Column("pressure_mmhg", sa.Numeric(precision=7, scale=2), nullable=True),
        sa.Column("pressure_unit", sa.String(length=8), nullable=True),
        sa.Column("is_valid", sa.Boolean(), nullable=False),
        sa.Column("quality_flags", sa.JSON(), nullable=False),
        sa.Column("firmware_version", sa.String(length=64), nullable=True),
        sa.Column("processing_algorithm_version", sa.String(length=64), nullable=True),
        sa.Column("data_provenance", sa.String(length=32), nullable=False),
        sa.Column("metadata_json", sa.JSON(), nullable=False),
        sa.CheckConstraint("event_sequence IS NULL OR event_sequence >= 0", name="ck_acoustic_events_event_sequence_nonnegative"),
        sa.CheckConstraint("device_timestamp_ms IS NULL OR device_timestamp_ms >= 0", name="ck_acoustic_events_device_time_nonnegative"),
        sa.CheckConstraint("strength IS NULL OR strength >= 0", name="ck_acoustic_events_strength_nonnegative"),
        sa.CheckConstraint("pressure_mmhg IS NULL OR pressure_mmhg BETWEEN 0 AND 400", name="ck_acoustic_events_pressure_range"),
        sa.CheckConstraint("(pressure_mmhg IS NULL AND pressure_unit IS NULL) OR (pressure_mmhg IS NOT NULL AND pressure_unit IS NOT NULL AND pressure_unit = 'mmHg')", name="ck_acoustic_events_pressure_unit_explicit"),
        sa.ForeignKeyConstraint(["batch_row_id"], ["telemetry_batches.id"], name="fk_acoustic_events_batch_row_id_telemetry_batches", ondelete="CASCADE"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_acoustic_events_device_id_devices", ondelete="RESTRICT"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_acoustic_events_session_id_measurement_sessions", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_acoustic_events"),
        sa.UniqueConstraint("device_id", "event_id", name="uq_acoustic_events_device_id_event_id"),
        sa.UniqueConstraint("session_id", "event_sequence", name="uq_acoustic_events_session_id_event_sequence"),
    )
    op.create_index("ix_acoustic_events_session_received", "acoustic_events", ["session_id", "received_at"])
    op.create_table(
        "reports",
        sa.Column("id", sa.Uuid(), nullable=False),
        sa.Column("session_id", sa.Uuid(), nullable=False),
        sa.Column("report_version", sa.String(length=32), nullable=False),
        sa.Column("generator_version", sa.String(length=64), nullable=False),
        sa.Column("status", sa.String(length=16), nullable=False),
        sa.Column("validation_status", sa.String(length=32), nullable=True),
        sa.Column("sample_count", sa.Integer(), nullable=True),
        sa.Column("input_watermark_sequence", sa.BigInteger(), nullable=True),
        sa.Column("failure_code", sa.String(length=64), nullable=True),
        sa.Column("created_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), server_default=sa.text("CURRENT_TIMESTAMP"), nullable=False),
        sa.Column("generated_at", sa.DateTime(timezone=True), nullable=True),
        sa.CheckConstraint("status IN ('pending', 'running', 'completed', 'failed')", name="ck_reports_status_valid"),
        sa.CheckConstraint("sample_count IS NULL OR sample_count >= 0", name="ck_reports_sample_count_nonnegative"),
        sa.ForeignKeyConstraint(["session_id"], ["measurement_sessions.id"], name="fk_reports_session_id_measurement_sessions", ondelete="CASCADE"),
        sa.PrimaryKeyConstraint("id", name="pk_reports"),
        sa.UniqueConstraint("session_id", "report_version", name="uq_reports_session_id_report_version"),
    )


def downgrade() -> None:
    op.drop_table("reports")
    op.drop_index("ix_acoustic_events_session_received", table_name="acoustic_events")
    op.drop_table("acoustic_events")
    op.drop_index("ix_telemetry_samples_session_received", table_name="telemetry_samples")
    op.drop_table("telemetry_samples")
    op.drop_index("ix_telemetry_batches_session_received", table_name="telemetry_batches")
    op.drop_table("telemetry_batches")
    op.drop_index("ix_measurement_sessions_device_created", table_name="measurement_sessions")
    op.drop_table("measurement_sessions")
    op.drop_table("devices")
