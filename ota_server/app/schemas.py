from typing import Literal

from pydantic import BaseModel, Field


class ApiResponse(BaseModel):
    code: int = 0
    message: str = "ok"


class RegisterRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=64)
    board_type: str = Field(..., min_length=1, max_length=64)
    hardware_version: str = Field(..., min_length=1, max_length=64)
    mac: str = Field(..., min_length=1, max_length=32)
    current_version: str = Field(..., min_length=1, max_length=64)


class ReleaseResponse(BaseModel):
    release_id: str
    board_type: str
    sys_version: str
    rollback_index: int
    package_url: str
    sha256: str
    signing_key_id: str | None = None
    mandatory: bool
    release_note: str | None = None


class CheckRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=64)
    board_type: str = Field(..., min_length=1, max_length=64)
    current_version: str = Field(..., min_length=1, max_length=64)


class CheckResponse(ApiResponse):
    has_update: bool
    release: ReleaseResponse | None = None


class ReportRequest(BaseModel):
    device_id: str = Field(..., min_length=1, max_length=64)
    release_id: str = Field(..., min_length=1, max_length=64)
    session_id: str = Field(..., min_length=1, max_length=64)
    stage: str = Field(..., min_length=1, max_length=32)
    result: Literal["started", "success", "failed"]
    current_version: str = Field(..., min_length=1, max_length=64)
    target_version: str = Field(..., min_length=1, max_length=64)
    reason_code: str | None = Field(default=None, max_length=64)
    reason_message: str | None = Field(default=None, max_length=256)
