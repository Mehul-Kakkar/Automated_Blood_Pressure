"""Public API error envelope."""

from pydantic import BaseModel


class ErrorDetail(BaseModel):
    location: str
    issue: str
    message: str


class ErrorBody(BaseModel):
    code: str
    message: str
    details: list[ErrorDetail]
    request_id: str


class ApiError(BaseModel):
    error: ErrorBody
