from __future__ import annotations

from datetime import datetime, timezone
from typing import Any
from uuid import uuid4

from sqlalchemy import (
    BigInteger,
    Boolean,
    CheckConstraint,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    JSON,
    String,
    Text,
    UniqueConstraint,
    text,
)
from sqlalchemy.orm import Mapped, mapped_column

from .database import Base


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def new_uuid() -> str:
    return str(uuid4())


class Draft(Base):
    __tablename__ = "drafts"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, default=1)
    document_encrypted: Mapped[str] = mapped_column(Text, nullable=False)
    edit_version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_by: Mapped[str] = mapped_column(String(320), nullable=False)


class LegacyDraftArchive(Base):
    __tablename__ = "legacy_draft_archives"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    schema_version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    document_encrypted: Mapped[str] = mapped_column(Text, nullable=False)
    archived_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    archived_by: Mapped[str] = mapped_column(String(320), nullable=False)


class Prompt(Base):
    __tablename__ = "prompts"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    name: Mapped[str] = mapped_column(String(120), nullable=False)
    version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    content_hash: Mapped[str] = mapped_column(String(64), nullable=False, unique=True, index=True)
    size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    duration_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    storage_name: Mapped[str] = mapped_column(String(80), nullable=False, unique=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    created_by: Mapped[str] = mapped_column(String(320), nullable=False)


class Revision(Base):
    __tablename__ = "revisions"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    schema_version: Mapped[int] = mapped_column(Integer, nullable=False, default=1)
    manifest_encrypted: Mapped[str] = mapped_column(Text, nullable=False)
    manifest_sha256: Mapped[str] = mapped_column(String(64), nullable=False, unique=True)
    signature_b64: Mapped[str] = mapped_column(Text, nullable=False)
    source_revision_id: Mapped[int | None] = mapped_column(
        ForeignKey("revisions.id"),
        nullable=True,
    )
    published_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    published_by: Mapped[str] = mapped_column(String(320), nullable=False)


class Device(Base):
    __tablename__ = "devices"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    display_name: Mapped[str] = mapped_column(String(120), nullable=False)
    token_hash: Mapped[str] = mapped_column(String(64), nullable=False, unique=True)
    app_version: Mapped[str] = mapped_column(String(80), default="unknown")
    helper_version: Mapped[str] = mapped_column(String(80), default="unknown")
    status: Mapped[dict[str, Any]] = mapped_column(JSON, default=dict)
    desired_revision_id: Mapped[int | None] = mapped_column(ForeignKey("revisions.id"))
    active_revision_id: Mapped[int | None] = mapped_column(ForeignKey("revisions.id"))
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    enrolled_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    revoked_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class PairingCode(Base):
    __tablename__ = "pairing_codes"

    id: Mapped[str] = mapped_column(String(36), primary_key=True, default=new_uuid)
    code_digest: Mapped[str] = mapped_column(String(64), nullable=False, unique=True)
    display_name: Mapped[str] = mapped_column(String(120), nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    failures: Mapped[int] = mapped_column(Integer, default=0)
    used_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    created_by: Mapped[str] = mapped_column(String(320), nullable=False)


class EnrollmentAttempt(Base):
    __tablename__ = "enrollment_attempts"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    source_ip: Mapped[str] = mapped_column(String(64), nullable=False, index=True)
    succeeded: Mapped[bool] = mapped_column(Boolean, default=False)
    attempted_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class CallRecord(Base):
    __tablename__ = "call_records"

    id: Mapped[str] = mapped_column(String(80), primary_key=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), nullable=False)
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    caller_encrypted: Mapped[str | None] = mapped_column(Text)
    caller_last4: Mapped[str] = mapped_column(String(4), default="")
    policy_decision: Mapped[str] = mapped_column(String(64), nullable=False)
    revision_id: Mapped[int | None] = mapped_column(Integer)
    menu_path: Mapped[list[str]] = mapped_column(JSON, default=list)
    result: Mapped[str] = mapped_column(String(64), nullable=False)
    duration_seconds: Mapped[int] = mapped_column(Integer, default=0)
    events: Mapped[list[dict[str, Any]]] = mapped_column(JSON, default=list)
    session_audit: Mapped[dict[str, Any] | None] = mapped_column(JSON)
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class SessionAuditPolicy(Base):
    __tablename__ = "session_audit_policies"

    version: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    enabled: Mapped[bool] = mapped_column(Boolean, nullable=False)
    local_quota_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    created_by: Mapped[str] = mapped_column(String(320), nullable=False)


class DeviceAuditPolicyAcknowledgement(Base):
    __tablename__ = "device_audit_policy_acknowledgements"

    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    policy_version: Mapped[int] = mapped_column(ForeignKey("session_audit_policies.version"), primary_key=True)
    applied_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class CallSafetyPolicy(Base):
    __tablename__ = "call_safety_policies"
    __table_args__ = (CheckConstraint("maximum_call_duration_seconds BETWEEN 60 AND 86400", name="ck_call_safety_duration"),)

    version: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    maximum_call_duration_seconds: Mapped[int] = mapped_column(Integer, nullable=False)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    created_by: Mapped[str] = mapped_column(String(320), nullable=False)


class DeviceCallSafetyAcknowledgement(Base):
    __tablename__ = "device_call_safety_acknowledgements"

    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), primary_key=True)
    # Zero denotes the signed default policy, so this cannot reference a policy row.
    policy_version: Mapped[int] = mapped_column(Integer, primary_key=True)
    maximum_call_duration_seconds: Mapped[int] = mapped_column(Integer, nullable=False)
    applied_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class RecordingRetentionPolicy(Base):
    __tablename__ = "recording_retention_policies"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, default=1)
    mode: Mapped[str] = mapped_column(String(16), nullable=False, default="automatic")
    days: Mapped[int] = mapped_column(Integer, nullable=False, default=30)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_by: Mapped[str] = mapped_column(String(320), nullable=False)


class Recording(Base):
    __tablename__ = "recordings"
    __table_args__ = (
        UniqueConstraint("device_id", "call_id", "sequence", name="uq_recording_call_sequence"),
        Index("uq_session_audit_call", "device_id", "call_id", unique=True,
              postgresql_where=text("kind = 'session_audit'"), sqlite_where=text("kind = 'session_audit'")),
        CheckConstraint("(kind = 'session_audit' AND block_id IS NULL) OR "
                        "(kind IN ('voicemail', 'conversation') AND block_id IS NOT NULL AND revision_id IS NOT NULL)",
                        name="ck_recording_identity"),
    )

    id: Mapped[str] = mapped_column(String(36), primary_key=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), nullable=False, index=True)
    call_id: Mapped[str] = mapped_column(ForeignKey("call_records.id"), nullable=False, index=True)
    revision_id: Mapped[int | None] = mapped_column(ForeignKey("revisions.id"))
    block_id: Mapped[str | None] = mapped_column(String(36))
    sequence: Mapped[int] = mapped_column(Integer, nullable=False)
    kind: Mapped[str] = mapped_column(String(24), nullable=False, default="voicemail", index=True)
    audit_metadata: Mapped[dict[str, Any] | None] = mapped_column(JSON)
    operator_encrypted: Mapped[str | None] = mapped_column(Text)
    operator_last4: Mapped[str] = mapped_column(String(4), nullable=False, default="")
    segment_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    captured_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    duration_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    stop_reason: Mapped[str] = mapped_column(String(32), nullable=False)
    source_size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    source_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    status: Mapped[str] = mapped_column(String(24), nullable=False, default="uploading", index=True)
    source_format: Mapped[str] = mapped_column(String(32), nullable=False, default="legacy_wav", server_default="legacy_wav")
    partial: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False, server_default=text("false"))
    processing_error: Mapped[str | None] = mapped_column(String(500))
    media_key_version: Mapped[int | None] = mapped_column(Integer)
    media_storage_name: Mapped[str | None] = mapped_column(String(80), unique=True)
    media_size_bytes: Mapped[int | None] = mapped_column(BigInteger)
    media_sha256: Mapped[str | None] = mapped_column(String(64))
    listened_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    deleted_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    deletion_reason: Mapped[str | None] = mapped_column(String(32))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class ContinuousRecording(Base):
    __tablename__ = "continuous_recordings"

    recording_id: Mapped[str] = mapped_column(ForeignKey("recordings.id", ondelete="CASCADE"), primary_key=True)
    manifest: Mapped[dict[str, Any]] = mapped_column(JSON, nullable=False)
    state: Mapped[str] = mapped_column(String(24), nullable=False, default="uploading", index=True)
    attempts: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    retry_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    accepted_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    operation_duration_ms: Mapped[int | None] = mapped_column(BigInteger)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class RecordingUpload(Base):
    __tablename__ = "recording_uploads"

    recording_id: Mapped[str] = mapped_column(
        ForeignKey("recordings.id", ondelete="CASCADE"),
        primary_key=True,
    )
    expected_size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    source_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    upload_offset: Mapped[int] = mapped_column(BigInteger, nullable=False, default=0)
    storage_name: Mapped[str] = mapped_column(String(80), nullable=False, unique=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)


class ConversationRecordingSegment(Base):
    __tablename__ = "conversation_recording_segments"
    __table_args__ = (
        UniqueConstraint(
            "recording_id",
            "segment_index",
            name="uq_conversation_recording_segment_index",
        ),
    )

    recording_id: Mapped[str] = mapped_column(
        ForeignKey("recordings.id", ondelete="CASCADE"),
        primary_key=True,
    )
    segment_index: Mapped[int] = mapped_column(Integer, primary_key=True)
    captured_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    duration_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    stop_reason: Mapped[str] = mapped_column(String(32), nullable=False)
    partial: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    expected_size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    source_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    upload_offset: Mapped[int] = mapped_column(BigInteger, nullable=False, default=0)
    storage_name: Mapped[str] = mapped_column(String(80), nullable=False, unique=True)
    status: Mapped[str] = mapped_column(String(24), nullable=False, default="uploading", index=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)


class DisplaySettings(Base):
    __tablename__ = "display_settings"
    __table_args__ = (
        CheckConstraint("id = 1", name="display_settings_singleton"),
        CheckConstraint("date_calendar IN ('gregorian', 'persian')", name="display_settings_calendar"),
    )

    id: Mapped[int] = mapped_column(Integer, primary_key=True, default=1)
    timezone: Mapped[str] = mapped_column(String(80), nullable=False, default="Asia/Tehran", server_default="Asia/Tehran")
    date_calendar: Mapped[str] = mapped_column(String(16), nullable=False, default="persian", server_default="persian")
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_by: Mapped[str] = mapped_column(String(320), nullable=False)


class NtfySettings(Base):
    __tablename__ = "ntfy_settings"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, default=1)
    enabled: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    server_url: Mapped[str] = mapped_column(String(200), nullable=False, default="https://ntfy.sh")
    topic: Mapped[str] = mapped_column(String(64), nullable=False, default="")
    token_encrypted: Mapped[str | None] = mapped_column(Text, nullable=True)
    events: Mapped[dict[str, Any]] = mapped_column(JSON, nullable=False)
    last_offline_notified_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    last_storage_notified_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    updated_by: Mapped[str] = mapped_column(String(320), nullable=False)


class AuditLog(Base):
    __tablename__ = "audit_logs"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    actor: Mapped[str] = mapped_column(String(320), nullable=False)
    action: Mapped[str] = mapped_column(String(120), nullable=False)
    target: Mapped[str] = mapped_column(String(160), nullable=False)
    details: Mapped[dict[str, Any]] = mapped_column(JSON, default=dict)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
