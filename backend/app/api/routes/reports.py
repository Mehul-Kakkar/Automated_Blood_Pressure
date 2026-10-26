"""Owner-scoped deterministic session report generation and downloads."""
from typing import Annotated
from uuid import UUID
from fastapi import APIRouter, Depends, HTTPException, Response, status
from fastapi.responses import Response as BinaryResponse
from sqlalchemy.orm import Session
from app.db.session import get_db_session
from app.models import Report
from app.schemas.reports import ReportResponse
from app.security.auth import get_current_owner_id
from app.services.reports import ReportService, generate_pdf
from app.services.session_lifecycle import owned_session

router = APIRouter(prefix="/api/v1/sessions", tags=["reports"])
Db = Annotated[Session, Depends(get_db_session)]
OwnerId = Annotated[UUID, Depends(get_current_owner_id)]

def _response(report: Report) -> ReportResponse:
    return ReportResponse(id=report.id, session_id=report.session_id, report_version=report.report_version,
        generator_version=report.generator_version, status=report.status, validation_status=report.validation_status,
        sample_count=report.sample_count, content_sha256=report.content_sha256, generated_at=report.generated_at,
        report=report.report_data)

def _owned_report(db: Session, owner_id: UUID, session_id: UUID) -> Report:
    if owned_session(db, owner_id, session_id) is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    report = db.query(Report).filter(Report.session_id == session_id, Report.report_version == "v1").one_or_none()
    if report is None or report.report_data is None:
        raise HTTPException(status_code=404, detail="Report has not been generated.")
    return report

@router.post("/{session_id}/report", response_model=ReportResponse, status_code=status.HTTP_201_CREATED)
def generate_session_report(session_id: UUID, response: Response, db: Db, owner_id: OwnerId) -> ReportResponse:
    result = owned_session(db, owner_id, session_id)
    if result is None:
        raise HTTPException(status_code=404, detail="Session was not found.")
    report, created = ReportService(db).get_or_create(*result)
    if not created:
        response.status_code = status.HTTP_200_OK
    return _response(report)

@router.get("/{session_id}/report", response_model=ReportResponse)
def get_session_report(session_id: UUID, db: Db, owner_id: OwnerId) -> ReportResponse:
    return _response(_owned_report(db, owner_id, session_id))

@router.get("/{session_id}/report.pdf", response_class=BinaryResponse)
def download_session_report(session_id: UUID, db: Db, owner_id: OwnerId) -> BinaryResponse:
    report = _owned_report(db, owner_id, session_id)
    assert report.report_data is not None
    return BinaryResponse(content=generate_pdf(report.report_data), media_type="application/pdf",
        headers={"Content-Disposition": f'attachment; filename="bp-report-{report.id}.pdf"',
                 "X-Report-SHA256": report.content_sha256 or ""})
