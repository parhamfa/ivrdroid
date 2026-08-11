from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Literal

from pydantic import Field, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="IVRDROID_",
        env_file=".env",
        extra="ignore",
    )

    environment: Literal["development", "test", "production"] = "development"
    database_url: str = "sqlite:///./ivrdroid.db"
    media_root: Path = Path("./var/prompts")
    recording_root: Path = Path("./var/recordings")
    public_base_url: str = "http://127.0.0.1:3200"
    auto_create_schema: bool = True

    prompt_quota_bytes: int = 1024 * 1024 * 1024
    prompt_upload_limit_bytes: int = 25 * 1024 * 1024
    prompt_duration_limit_seconds: int = 300
    revision_asset_limit_bytes: int = 128 * 1024 * 1024
    recording_quota_bytes: int = 5 * 1024 * 1024 * 1024
    recording_upload_limit_bytes: int = 40 * 1024 * 1024
    recording_chunk_limit_bytes: int = 1024 * 1024
    recording_abandoned_upload_hours: int = 24

    config_signing_private_key_b64: str = ""
    data_encryption_key_b64: str = ""
    pairing_hmac_key_b64: str = ""
    recording_encryption_key_b64: str = ""
    recording_encryption_key_version: int = Field(default=1, ge=1, le=2_147_483_647)
    recording_encryption_previous_keys_json: str = "{}"

    cf_access_team_domain: str = ""
    cf_access_audience: str = ""
    cf_device_service_client_id: str = ""
    cf_device_service_client_secret: str = ""
    development_admin_email: str = "owner@localhost"

    enrollment_code_ttl_seconds: int = 600
    enrollment_failure_window_seconds: int = 900
    enrollment_max_failures: int = 5

    @field_validator("public_base_url")
    @classmethod
    def strip_trailing_slash(cls, value: str) -> str:
        return value.rstrip("/")

    @field_validator(
        "config_signing_private_key_b64",
        "data_encryption_key_b64",
        "pairing_hmac_key_b64",
        "recording_encryption_key_b64",
        "recording_encryption_previous_keys_json",
    )
    @classmethod
    def require_production_secrets(cls, value: str, info):
        # Cross-field enforcement happens in validate_runtime so tests can build
        # settings incrementally without ever accepting empty production keys.
        return value.strip()

    def validate_runtime(self) -> None:
        self.media_root.mkdir(parents=True, exist_ok=True)
        self.recording_root.mkdir(parents=True, exist_ok=True)
        if self.environment == "production":
            required = {
                "config signing key": self.config_signing_private_key_b64,
                "data encryption key": self.data_encryption_key_b64,
                "pairing HMAC key": self.pairing_hmac_key_b64,
                "recording encryption key": self.recording_encryption_key_b64,
                "Cloudflare Access team domain": self.cf_access_team_domain,
                "Cloudflare Access audience": self.cf_access_audience,
                "Cloudflare device service client ID": self.cf_device_service_client_id,
                "Cloudflare device service client secret": self.cf_device_service_client_secret,
            }
            missing = [name for name, value in required.items() if not value]
            if missing:
                raise RuntimeError(f"Missing production settings: {', '.join(missing)}")
            if self.auto_create_schema:
                raise RuntimeError("Production must run Alembic; auto_create_schema must be false")


@lru_cache(maxsize=1)
def load_settings() -> Settings:
    return Settings()
