"""Build deterministic report snapshots and render experimental session PDFs."""

from __future__ import annotations

import hashlib
import json
import logging
from datetime import datetime, timezone
from decimal import Decimal
from io import BytesIO
from typing import Any, Sequence
from uuid import UUID

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    BaseDocTemplate, Frame, PageTemplate, Paragraph, Spacer, Table, TableStyle,
)
from reportlab.graphics.shapes import Drawing, Line, PolyLine, String
from reportlab.platypus import Flowable
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.models import AcousticEvent, Device, MeasurementSession, Report, SessionEvent, TelemetrySample

logger = logging.getLogger("api.reports")
REPORT_VERSION = "v1"
GENERATOR_VERSION = "bp-report/1.0.0"
TRACE_LIMIT = 20000


def _iso(value: datetime | None) -> str | None:
    if value is None:
        return None
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _num(value: Any) -> float | int | None:
    if value is None:
        return None
    if isinstance(value, Decimal):
        return float(value)
    return value


def _record_time(row: TelemetrySample | AcousticEvent) -> str | None:
    return _iso(row.device_recorded_at) or (f"device_uptime_ms:{row.device_timestamp_ms}" if row.device_timestamp_ms is not None else None)


def build_report_data(
    report: Report,
    session: MeasurementSession,
    device: Device,
    samples: Sequence[TelemetrySample],
    acoustic_events: Sequence[AcousticEvent],
    session_events: Sequence[SessionEvent],
    total_sample_count: int,
    generated_at: datetime,
    total_acoustic_event_count: int | None = None,
    total_session_event_count: int | None = None,
) -> dict[str, Any]:
    """Produce a JSON-safe snapshot using stored rows only; no clinical estimator exists."""
    usable_samples = [sample for sample in samples if sample.is_valid]
    calibrated = device.pressure_calibration_confirmed and bool(device.pressure_calibration_version)
    pressure = [
        {"time": _record_time(row), "device_timestamp_ms": row.device_timestamp_ms,
         "sample_id": str(row.sample_id), "sequence": row.sample_sequence, "mmHg": _num(row.pressure_mmhg),
         "valid": row.is_valid, "quality_flags": list(row.quality_flags or []),
         "calibration_version": row.synchronization_metadata.get("pressure_calibration_version")}
        for row in samples if row.pressure_mmhg is not None and row.pressure_unit == "mmHg" and calibrated
        and row.synchronization_metadata.get("pressure_calibration_version") == device.pressure_calibration_version
        and row.is_valid
    ]
    heart = [
        {"time": _record_time(row), "sequence": row.sample_sequence, "bpm": row.heart_rate_bpm,
         "quality_flags": list(row.quality_flags or [])}
        for row in samples if row.is_valid and row.heart_rate_bpm is not None
    ]
    envelope = [
        {"time": _record_time(row), "device_timestamp_ms": row.device_timestamp_ms,
         "sequence": row.sample_sequence, "value": _num(row.acoustic_envelope),
         "valid": row.is_valid, "quality_flags": list(row.quality_flags or [])}
        for row in samples if row.acoustic_envelope is not None
    ]
    pressure_by_device_time = {row.device_timestamp_ms: row for row in samples
                               if calibrated and row.synchronization_metadata.get("pressure_calibration_version") == device.pressure_calibration_version
                               and row.is_valid and row.pressure_mmhg is not None
                               and row.pressure_unit == "mmHg" and row.device_timestamp_ms is not None}
    events = [
        {"id": str(row.event_id), "event_sequence": row.event_sequence, "type": row.event_type,
         "time": _record_time(row), "device_timestamp_ms": row.device_timestamp_ms,
         "strength": _num(row.strength), "valid": row.is_valid,
         "quality_flags": list(row.quality_flags or []), "algorithm_version": row.processing_algorithm_version,
         "pressure_association": ({"method": "exact_device_timestamp", "sample_id": str(pressure_by_device_time[row.device_timestamp_ms].sample_id),
                                   "mmHg": _num(pressure_by_device_time[row.device_timestamp_ms].pressure_mmhg)}
                                  if row.is_valid and row.device_timestamp_ms in pressure_by_device_time else None)}
        for row in acoustic_events
    ]
    gaps: list[dict[str, Any]] = []
    stale_or_quality: list[dict[str, Any]] = []
    for row in samples:
        flags = list(row.quality_flags or [])
        if flags:
            stale_or_quality.append({"sample_id": str(row.sample_id), "flags": flags, "valid": row.is_valid})
    for event in session_events:
        if event.event_type == "telemetry_batch":
            observed = event.data.get("observed_sequence_gaps", [])
            if isinstance(observed, list):
                gaps.extend({"from_sequence": gap.get("from_sequence"), "to_sequence": gap.get("to_sequence"),
                             "source": "stored_sequence_gap", "provisional": bool(event.data.get("gap_is_provisional", True))}
                            for gap in observed if isinstance(gap, dict))
        if event.event_type in {"telemetry_gap", "data_gap", "device_disconnected"}:
            gaps.append({"event_type": event.event_type, "server_received_at": _iso(event.received_at), "details": event.data})
    limitations = ["Experimental system. Not clinically validated; this report is not a diagnosis or treatment recommendation.",
                   "Systolic and diastolic blood pressure are unavailable: no validated estimator is implemented.",
                   "Acoustic waveform values are stored envelope samples only; no audio-rate waveform is reconstructed."]
    if not calibrated and any(sample.pressure_mmhg is not None for sample in samples):
        limitations.append("Pressure values were withheld because device pressure calibration is not confirmed.")
    if not pressure:
        limitations.append("No validated cuff-pressure samples are available for this report.")
    if not heart:
        limitations.append("No valid heart-rate samples are available.")
    if session.lifecycle != "completed":
        limitations.append(f"Session lifecycle is '{session.lifecycle}', not device-confirmed completed.")
    if not usable_samples:
        limitations.append("No valid telemetry samples are available; session is uninterpretable from stored samples.")
    unique_flags = sorted({flag for sample in samples for flag in (sample.quality_flags or [])})
    duration_seconds = None
    if session.started_at and session.ended_at:
        duration_seconds = max(0.0, (session.ended_at - session.started_at).total_seconds())
    heart_summary = None
    if heart:
        values = [item["bpm"] for item in heart]
        heart_summary = {"method": "arithmetic summary of valid stored sample values; not clinically interpreted",
                         "count": len(values), "minimum_bpm": min(values), "maximum_bpm": max(values),
                         "mean_bpm": round(sum(values) / len(values), 2)}
    times_known = all(item.get("device_timestamp_ms") is not None for item in envelope)
    return {
        "report_id": str(report.id), "session_id": str(session.id), "report_version": REPORT_VERSION,
        "generator_version": GENERATOR_VERSION, "generated_at": _iso(generated_at),
        "experimental_notice": "EXPERIMENTAL — NOT CLINICALLY VALIDATED",
        "session": {"lifecycle": session.lifecycle, "created_at": _iso(session.created_at),
                    "started_at": _iso(session.started_at), "ended_at": _iso(session.ended_at),
                    "duration_seconds": duration_seconds, "termination_reason": session.termination_reason,
                    "data_provenance": session.data_provenance},
        "device": {"id": str(device.id), "identifier": device.device_identifier,
                   "hardware_revision": device.hardware_revision,
                   "firmware_version": session.firmware_version or device.firmware_version,
                   "processing_algorithm_version": session.processing_algorithm_version or device.processing_algorithm_version,
                   "pressure_calibration_confirmed": bool(calibrated),
                   "pressure_calibration_version": device.pressure_calibration_version if calibrated else None},
        "data_completeness": {"stored_sample_count": total_sample_count, "included_sample_count": len(samples),
                              "valid_sample_count": sum(sample.is_valid for sample in samples),
                              "trace_truncated": total_sample_count > len(samples),
                              "acoustic_event_count": total_acoustic_event_count if total_acoustic_event_count is not None else len(events),
                              "included_acoustic_event_count": len(events),
                              "acoustic_event_list_truncated": (total_acoustic_event_count or len(events)) > len(events),
                              "session_event_count": total_session_event_count if total_session_event_count is not None else len(session_events),
                              "session_event_list_truncated": (total_session_event_count or len(session_events)) > len(session_events),
                              "sequence_or_transmission_gaps": gaps,
                              "quality_flagged_samples": stale_or_quality, "quality_flags": unique_flags,
                              "stale_samples": sum("stale" in (sample.quality_flags or []) for sample in samples),
                              "receipt_timing_note": "Device and server timestamps are preserved separately; receipt delay is not classified without a synchronized device clock."},
        "pressure_trace": {"unit": "mmHg", "calibration_confirmed": bool(calibrated), "samples": pressure},
        "acoustic_envelope": {"samples": envelope,
                              "resolution": "stored device timestamp intervals" if times_known else "stored sample records; sampling interval unspecified",
                              "interpolation": False},
        "acoustic_events": events,
        "heart_rate": {"samples": heart, "summary": heart_summary},
        "derived_measurements": {"systolic_mmhg": None, "diastolic_mmhg": None,
                                  "method": None, "validation_status": "unavailable_no_validated_estimator"},
        "interpretability": {"status": "uninterpretable" if not usable_samples else ("partial" if session.lifecycle != "completed" or not pressure else "signal_data_only"),
                             "reasons": limitations[1:] if (not usable_samples or not pressure or session.lifecycle != "completed") else []},
        "limitations": limitations,
    }


def canonical_json(data: dict[str, Any]) -> bytes:
    return json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8")


class SignalChart(Flowable):
    """Small vector chart preserving actual stored point spacing/order without interpolation."""
    def __init__(self, title: str, points: list[tuple[float, float]], unit: str,
                 markers: list[tuple[float, float]] | None = None, x_unit: str = "sample order") -> None:
        super().__init__()
        self.title, self.points, self.unit, self.x_unit = title, points, unit, x_unit
        self.markers = markers or []
        self.width, self.height = 170 * mm, 55 * mm

    def draw(self) -> None:
        c = self.canv
        c.setFont("Helvetica-Bold", 9)
        c.drawString(0, self.height - 10, self.title)
        left, bottom, plot_w, plot_h = 12 * mm, 10 * mm, self.width - 20 * mm, self.height - 24 * mm
        c.setStrokeColor(colors.grey)
        c.line(left, bottom, left, bottom + plot_h)
        c.line(left, bottom, left + plot_w, bottom)
        c.setFont("Helvetica", 7)
        c.drawString(left, 1, f"X: {self.x_unit}  |  Y: {self.unit}  |  Stored records; no interpolation")
        if not self.points:
            c.drawString(left + 8, bottom + plot_h / 2, "No eligible samples")
            return
        xs, ys = [p[0] for p in self.points], [p[1] for p in self.points]
        xmin, xmax = min(xs), max(xs)
        ymin, ymax = min(ys), max(ys)
        if xmax == xmin: xmax = xmin + 1
        if ymax == ymin: ymax = ymin + 1
        c.drawRightString(left - 2, bottom + plot_h - 2, f"{max(ys):.3g}")
        c.drawRightString(left - 2, bottom, f"{min(ys):.3g}")
        c.drawString(left, bottom - 8, f"{xmin:.3g}")
        c.drawRightString(left + plot_w, bottom - 8, f"{xmax:.3g}")
        coords = [(left + (x - xmin) / (xmax - xmin) * plot_w,
                   bottom + (y - ymin) / (ymax - ymin) * plot_h) for x, y in self.points]
        c.setStrokeColor(colors.HexColor("#176b87"))
        c.setLineWidth(1)
        if len(coords) == 1:
            c.circle(coords[0][0], coords[0][1], 1.4 * mm, stroke=1, fill=0)
        else:
            path = c.beginPath()
            path.moveTo(*coords[0])
            for coord in coords[1:]: path.lineTo(*coord)
            c.drawPath(path, stroke=1, fill=0)
        c.setFillColor(colors.HexColor("#bd3b35"))
        for x, y in self.markers:
            if xmin <= x <= xmax and ymin <= y <= ymax:
                px = left + (x - xmin) / (xmax - xmin) * plot_w
                py = bottom + (y - ymin) / (ymax - ymin) * plot_h
                c.circle(px, py, 1.5 * mm, stroke=0, fill=1)


def _chart_points(report: dict[str, Any], key: str, value_key: str) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for index, item in enumerate(report[key]["samples"]):
        value = item.get(value_key)
        if value is None:
            continue
        x = item.get("device_timestamp_ms")
        points.append((float(x if x is not None else index), float(value)))
    return points


def _event_pressure_markers(report: dict[str, Any]) -> list[tuple[float, float]]:
    return [(float(event["device_timestamp_ms"]), float(event["pressure_association"]["mmHg"]))
            for event in report["acoustic_events"]
            if event.get("device_timestamp_ms") is not None and event.get("pressure_association") is not None]


def _timestamp_x_unit(samples: list[dict[str, Any]]) -> str:
    if samples and all(item.get("device_timestamp_ms") is not None for item in samples):
        return "device uptime (ms)"
    return "sample order / available device uptime"


def generate_pdf(report: dict[str, Any]) -> bytes:
    """Render a server-side, paginated PDF from an immutable report snapshot."""
    buffer = BytesIO()
    styles = getSampleStyleSheet()
    styles.add(ParagraphStyle(name="CenterTitle", parent=styles["Title"], alignment=TA_CENTER, textColor=colors.HexColor("#17324d")))
    styles.add(ParagraphStyle(name="Small", parent=styles["BodyText"], fontSize=8, leading=10))
    doc = BaseDocTemplate(buffer, pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm,
                          topMargin=18 * mm, bottomMargin=18 * mm, title=f"Experimental Session {report['session_id']}")
    frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="normal")
    def footer(canvas, document):
        canvas.saveState()
        canvas.setFont("Helvetica", 8)
        canvas.setFillColor(colors.grey)
        canvas.drawString(18 * mm, 10 * mm, f"Report {report['report_id']}  |  Session {report['session_id']}")
        canvas.drawRightString(A4[0] - 18 * mm, 10 * mm, f"Page {document.page}")
        canvas.restoreState()
    doc.addPageTemplates([PageTemplate(id="report", frames=[frame], onPage=footer)])
    story: list[Flowable] = [Paragraph("Experimental Measurement Report", styles["CenterTitle"]),
        Paragraph(report["experimental_notice"], styles["Heading2"]),
        Paragraph("This report presents stored experimental signal data only. It is not clinically validated and is not a diagnosis.", styles["BodyText"]), Spacer(1, 5 * mm)]
    session, device = report["session"], report["device"]
    rows = [["Report ID", report["report_id"]], ["Session ID", report["session_id"]],
            ["Device", f"{device['identifier']} ({device['id']})"], ["Hardware", device.get("hardware_revision") or "Unavailable"],
            ["Firmware", device.get("firmware_version") or "Unavailable"],
            ["Processing algorithm", device.get("processing_algorithm_version") or "Unavailable"],
            ["Lifecycle", session["lifecycle"]], ["Started / ended (UTC)", f"{session['started_at'] or 'Unavailable'} / {session['ended_at'] or 'Unavailable'}"],
            ["Duration", f"{session['duration_seconds']} seconds" if session["duration_seconds"] is not None else "Unavailable"],
            ["Generated (UTC)", report["generated_at"]]]
    table = Table(rows, colWidths=[45 * mm, 120 * mm], repeatRows=0, hAlign="LEFT")
    table.setStyle(TableStyle([("BACKGROUND", (0,0),(0,-1),colors.HexColor("#e8eef2")),("GRID",(0,0),(-1,-1),0.3,colors.lightgrey),
        ("VALIGN",(0,0),(-1,-1),"TOP"),("FONTNAME",(0,0),(0,-1),"Helvetica-Bold"),("FONTSIZE",(0,0),(-1,-1),8),("WORDWRAP",(0,0),(-1,-1),"CJK")]))
    story.extend([Paragraph("Session and device", styles["Heading1"]), table, Spacer(1, 4 * mm)])
    story.append(Paragraph("Pressure trace (mmHg)", styles["Heading1"]))
    if report["pressure_trace"]["calibration_confirmed"]:
        story.append(SignalChart("Stored pressure samples; red markers = exact-time acoustic events",
                                 _chart_points(report, "pressure_trace", "mmHg"), "mmHg",
                                 _event_pressure_markers(report),
                                 _timestamp_x_unit(report["pressure_trace"]["samples"])))
    else:
        story.append(Paragraph("No pressure trace shown: calibration is not confirmed.", styles["BodyText"]))
    story.append(Paragraph("Acoustic envelope", styles["Heading1"]))
    story.append(Paragraph(f"Resolution: {report['acoustic_envelope']['resolution']}; no interpolation.", styles["Small"]))
    story.append(SignalChart("Stored envelope values", _chart_points(report, "acoustic_envelope", "value"), "device units",
                             x_unit=_timestamp_x_unit(report["acoustic_envelope"]["samples"])))
    story.append(Paragraph("Detected acoustic event markers", styles["Heading1"]))
    event_rows = [["Event ID", "Type", "Device time", "Strength", "Valid", "Pressure link"]]
    event_rows.extend([[event["id"], event["type"], event["time"] or "Unavailable", str(event["strength"] or "Unavailable"),
                        str(event["valid"]),
                        (f"{event['pressure_association']['mmHg']} mmHg (exact time)" if event.get("pressure_association") else "None")]
                       for event in report["acoustic_events"][:300]])
    et = Table(event_rows, colWidths=[35*mm,24*mm,44*mm,22*mm,15*mm,25*mm], repeatRows=1)
    et.setStyle(TableStyle([("BACKGROUND",(0,0),(-1,0),colors.HexColor("#dbe8ef")),("GRID",(0,0),(-1,-1),0.25,colors.lightgrey),
                            ("FONTSIZE",(0,0),(-1,-1),6.5),("VALIGN",(0,0),(-1,-1),"TOP")]))
    story.extend([et, Paragraph("Only the first 300 event rows are rendered in the PDF; the JSON snapshot retains the first 1,000 stored events.", styles["Small"])])
    story.append(Paragraph("Heart rate", styles["Heading1"]))
    heart = report["heart_rate"]
    if heart["summary"]:
        s = heart["summary"]
        story.append(Paragraph(f"Valid stored samples: {s['count']}; range {s['minimum_bpm']}–{s['maximum_bpm']} bpm; arithmetic mean {s['mean_bpm']} bpm.", styles["BodyText"]))
        story.append(Paragraph(s["method"], styles["Small"]))
    else:
        story.append(Paragraph("No valid heart-rate data available.", styles["BodyText"]))
    story.append(Paragraph("Data completeness and quality", styles["Heading1"]))
    comp = report["data_completeness"]
    story.append(Paragraph(f"Stored samples: {comp['stored_sample_count']}; included: {comp['included_sample_count']}; valid: {comp['valid_sample_count']}; stale-flagged: {comp['stale_samples']}; events: {comp['acoustic_event_count']}; trace truncated: {comp['trace_truncated']}.", styles["BodyText"]))
    story.append(Paragraph(comp["receipt_timing_note"], styles["Small"]))
    story.append(Paragraph("Derived measurements", styles["Heading1"]))
    story.append(Paragraph("Systolic and diastolic pressure: unavailable. No validated estimator is implemented.", styles["BodyText"]))
    story.append(Paragraph("Limitations", styles["Heading1"]))
    for limitation in report["limitations"]:
        story.append(Paragraph(f"• {limitation}", styles["BodyText"]))
    doc.build(story)
    return buffer.getvalue()


class ReportService:
    """Coordinates snapshot persistence; routing and authorization stay outside this service."""
    def __init__(self, db: Session) -> None:
        self.db = db

    def get_or_create(self, session: MeasurementSession, device: Device) -> tuple[Report, bool]:
        report = self.db.scalar(select(Report).where(Report.session_id == session.id, Report.report_version == REPORT_VERSION))
        if report is not None and report.report_data is not None:
            return report, False
        if report is None:
            report = Report(session_id=session.id, report_version=REPORT_VERSION, generator_version=GENERATOR_VERSION,
                            status="running")
            self.db.add(report)
            self.db.flush()
        total = self.db.query(TelemetrySample).filter(TelemetrySample.session_id == session.id).count()
        samples = self.db.scalars(select(TelemetrySample).where(TelemetrySample.session_id == session.id)
                                  .order_by(TelemetrySample.sample_sequence.asc().nullslast(), TelemetrySample.received_at, TelemetrySample.id)
                                  .limit(TRACE_LIMIT + 1)).all()
        samples = samples[:TRACE_LIMIT]
        acoustic_events = self.db.scalars(select(AcousticEvent).where(AcousticEvent.session_id == session.id)
                                          .order_by(AcousticEvent.event_sequence.asc().nullslast(), AcousticEvent.received_at, AcousticEvent.id)
                                          .limit(1001)).all()
        total_acoustic_events = self.db.query(AcousticEvent).filter(AcousticEvent.session_id == session.id).count()
        event_rows = self.db.scalars(select(SessionEvent).where(SessionEvent.session_id == session.id)
                                     .order_by(SessionEvent.id).limit(10001)).all()
        total_session_events = self.db.query(SessionEvent).filter(SessionEvent.session_id == session.id).count()
        now = datetime.now(timezone.utc)
        data = build_report_data(report, session, device, samples, acoustic_events[:1000], event_rows, total, now,
                                 total_acoustic_events, total_session_events)
        report.report_data = data
        report.content_sha256 = hashlib.sha256(canonical_json(data)).hexdigest()
        report.sample_count = total
        report.input_watermark_sequence = max((sample.sample_sequence for sample in samples if sample.sample_sequence is not None), default=None)
        report.validation_status = data["interpretability"]["status"]
        report.status = "completed"
        report.generated_at = now
        self.db.commit()
        self.db.refresh(report)
        logger.info("report_generated", extra={"event": "report_generated", "sample_count": total})
        return report, True
