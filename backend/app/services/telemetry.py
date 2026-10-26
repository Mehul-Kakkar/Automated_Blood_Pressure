"""Transactional telemetry batch persistence with content-aware idempotency."""

import hashlib
from dataclasses import dataclass
from datetime import datetime, timezone
from uuid import UUID

from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.core.config import get_settings
from app.models import AcousticEvent, Device, MeasurementSession, SessionEvent, TelemetryBatch, TelemetrySample
from app.schemas.telemetry import TelemetryBatchInput


class TelemetryIngestError(Exception):
    """Base class for expected telemetry persistence errors."""


class TelemetryNotFoundError(TelemetryIngestError):
    """Device or session does not exist or does not belong to the device."""


class TelemetryBatchConflictError(TelemetryIngestError):
    """A batch or record identifier was reused with different content."""


class TelemetryPayloadTooLargeError(TelemetryIngestError):
    """Canonical batch payload exceeded its configured byte limit."""


@dataclass(frozen=True)
class TelemetryIngestResult:
    batch_id: UUID
    duplicate: bool
    sample_count: int
    event_count: int
    received_at: datetime


def ingest_telemetry_batch(
    db: Session,
    payload: TelemetryBatchInput,
    *,
    max_samples: int | None = None,
    max_bytes: int | None = None,
    pressure_calibration_version: str | None = None,
) -> TelemetryIngestResult:
    """Store a batch atomically; an exact repeated batch is a no-op."""
    settings = get_settings()
    max_samples = max_samples if max_samples is not None else settings.max_telemetry_batch_samples
    max_bytes = max_bytes if max_bytes is not None else settings.max_telemetry_batch_bytes
    if len(payload.samples) > max_samples:
        raise TelemetryPayloadTooLargeError(f"batch exceeds the {max_samples}-sample limit")
    try:
        canonical = payload.validate_size(max_bytes)
    except ValueError as exc:
        raise TelemetryPayloadTooLargeError(str(exc)) from exc
    digest = hashlib.sha256(canonical).hexdigest()
    now = datetime.now(timezone.utc)

    try:
        with db.begin():
            device = db.get(Device, payload.device_id)
            measurement_session = db.get(MeasurementSession, payload.session_id)
            if device is None or measurement_session is None or measurement_session.device_id != payload.device_id:
                raise TelemetryNotFoundError("Registered device and matching session are required.")

            existing = db.scalar(
                select(TelemetryBatch).where(
                    TelemetryBatch.device_id == payload.device_id,
                    TelemetryBatch.batch_id == payload.batch_id,
                )
            )
            if existing is not None:
                if existing.payload_sha256 != digest:
                    raise TelemetryBatchConflictError("Batch ID was already used with different content.")
                return TelemetryIngestResult(
                    payload.batch_id, True, existing.sample_count, existing.event_count, existing.received_at
                )

            prior_sequence = db.scalar(
                select(TelemetrySample.sample_sequence)
                .where(TelemetrySample.session_id == payload.session_id, TelemetrySample.sample_sequence.is_not(None))
                .order_by(TelemetrySample.sample_sequence.desc())
                .limit(1)
            )
            sequence_values = sorted(
                sample.sample_sequence for sample in payload.samples if sample.sample_sequence is not None
            )
            observed_gaps: list[dict[str, int]] = []
            if sequence_values:
                expected = (prior_sequence + 1) if prior_sequence is not None else sequence_values[0]
                for sequence in sequence_values:
                    if sequence > expected:
                        observed_gaps.append({"from_sequence": expected, "to_sequence": sequence - 1})
                    expected = max(expected, sequence + 1)

            batch_row = TelemetryBatch(
                batch_id=payload.batch_id,
                device_id=payload.device_id,
                session_id=payload.session_id,
                payload_sha256=digest,
                sample_count=len(payload.samples),
                event_count=len(payload.acoustic_events),
                schema_version=payload.schema_version,
                firmware_version=payload.firmware_version,
                processing_algorithm_version=payload.processing_algorithm_version,
                data_provenance=payload.data_provenance,
                sync_status="received",
            )
            db.add(batch_row)
            db.flush()
            db.add_all(
                [
                        TelemetrySample(
                            sample_id=item.sample_id,
                            batch_row_id=batch_row.id,
                            device_id=payload.device_id,
                            session_id=payload.session_id,
                            sample_sequence=item.sample_sequence,
                            device_timestamp_ms=item.device_timestamp_ms,
                            device_recorded_at=item.device_recorded_at,
                            pressure_mmhg=item.pressure_mmhg,
                            pressure_unit="mmHg" if item.pressure_mmhg is not None else None,
                            heart_rate_bpm=item.heart_rate_bpm,
                            raw_adc=item.raw_adc,
                            filtered=item.filtered,
                            acoustic_envelope=item.acoustic_envelope,
                            noise_floor=item.noise_floor,
                            threshold=item.threshold,
                            event_detected=item.event_detected,
                            is_valid=item.is_valid,
                            quality_flags=item.quality_flags,
                            firmware_version=payload.firmware_version,
                            processing_algorithm_version=payload.processing_algorithm_version,
                            data_provenance=payload.data_provenance,
                            synchronization_metadata={
                                "batch_id": str(payload.batch_id),
                                "received_at": now.isoformat(),
                                "pressure_calibration_version": pressure_calibration_version,
                            },
                        )
                        for item in payload.samples
                ]
            )
            db.add_all(
                [
                        AcousticEvent(
                            event_id=item.event_id,
                            batch_row_id=batch_row.id,
                            device_id=payload.device_id,
                            session_id=payload.session_id,
                            event_sequence=item.event_sequence,
                            event_type=item.event_type,
                            device_timestamp_ms=item.device_timestamp_ms,
                            device_recorded_at=item.device_recorded_at,
                            strength=item.strength,
                            pressure_mmhg=item.pressure_mmhg,
                            pressure_unit="mmHg" if item.pressure_mmhg is not None else None,
                            is_valid=item.is_valid,
                            quality_flags=item.quality_flags,
                            firmware_version=payload.firmware_version,
                            processing_algorithm_version=payload.processing_algorithm_version,
                            data_provenance=payload.data_provenance,
                            metadata_json=item.metadata,
                        )
                        for item in payload.acoustic_events
                ]
            )
            recorded_values = [sample.device_recorded_at for sample in payload.samples if sample.device_recorded_at]
            db.add(
                SessionEvent(
                    event_id=payload.batch_id,
                    device_id=payload.device_id,
                    session_id=payload.session_id,
                    event_type="telemetry_batch",
                    payload_sha256=digest,
                    device_sequence=max(sequence_values) if sequence_values else None,
                    device_recorded_at=max(recorded_values) if recorded_values else None,
                    data={
                        "batch_id": str(payload.batch_id),
                        "sample_count": len(payload.samples),
                        "acoustic_event_count": len(payload.acoustic_events),
                        "first_sequence": min(sequence_values) if sequence_values else None,
                        "last_sequence": max(sequence_values) if sequence_values else None,
                        "observed_sequence_gaps": observed_gaps,
                        "sequence_out_of_order": prior_sequence is not None and any(
                            sequence <= prior_sequence for sequence in sequence_values
                        ),
                        "gap_is_provisional": bool(observed_gaps),
                    },
                )
            )
            db.flush()
            received_at = batch_row.received_at or now
            result = TelemetryIngestResult(
                payload.batch_id, False, len(payload.samples), len(payload.acoustic_events), received_at
            )
        return result
    except IntegrityError as exc:
        # A concurrent request can win the unique batch key; all child rows
        # from this transaction are rolled back before we inspect that winner.
        existing = db.scalar(
            select(TelemetryBatch).where(
                TelemetryBatch.device_id == payload.device_id,
                TelemetryBatch.batch_id == payload.batch_id,
            )
        )
        if existing is not None:
            if existing.payload_sha256 == digest:
                return TelemetryIngestResult(
                    payload.batch_id, True, existing.sample_count, existing.event_count, existing.received_at
                )
            raise TelemetryBatchConflictError("Batch ID was already used with different content.") from exc
        raise TelemetryBatchConflictError("Telemetry record identifiers have already been used.") from exc
