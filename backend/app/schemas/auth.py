"""Registration, login, and opaque bearer token contracts."""

from pydantic import BaseModel, ConfigDict, Field, field_validator

USERNAME_PATTERN = r"^[a-z0-9_.-]{3,48}$"


class CredentialsRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    username: str = Field(min_length=3, max_length=48, pattern=USERNAME_PATTERN)
    password: str = Field(min_length=12, max_length=128)

    @field_validator("username", mode="before")
    @classmethod
    def normalize_username(cls, value: object) -> object:
        return value.strip().lower() if isinstance(value, str) else value


class RegistrationRequest(CredentialsRequest):
    display_name: str = Field(min_length=1, max_length=128)

    @field_validator("display_name")
    @classmethod
    def normalize_display_name(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("Display name cannot be blank.")
        return value


class PasswordUpdateRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    password: str = Field(min_length=12, max_length=128)


class AuthResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"
    expires_at: str
    username: str
    display_name: str
