"""Authenticated owner profile request and response contracts."""

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator


class ProfileResponse(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    username: str
    display_name: str
    created_at: datetime
    avatar_available: bool


class ProfileUpdateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    display_name: str = Field(min_length=1, max_length=128)

    @field_validator("display_name")
    @classmethod
    def non_blank_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Display name cannot be blank.")
        return value


class AvatarUpdateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    content_type: Literal["image/png", "image/jpeg", "image/webp", "image/gif"]
    data_base64: str = Field(min_length=1, max_length=1_398_104)
