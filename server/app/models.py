from __future__ import annotations

from datetime import datetime, timezone
from typing import Any
from uuid import uuid4

from sqlalchemy import (
    BigInteger,
    Boolean,
    DateTime,
    ForeignKey,
    Integer,
    JSON,
    String,
    Text,
    UniqueConstraint,
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
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


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
    )

    id: Mapped[str] = mapped_column(String(36), primary_key=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), nullable=False, index=True)
    call_id: Mapped[str] = mapped_column(ForeignKey("call_records.id"), nullable=False, index=True)
    revision_id: Mapped[int] = mapped_column(ForeignKey("revisions.id"), nullable=False)
    block_id: Mapped[str] = mapped_column(String(36), nullable=False)
    sequence: Mapped[int] = mapped_column(Integer, nullable=False)
    kind: Mapped[str] = mapped_column(String(24), nullable=False, default="voicemail", index=True)
    operator_encrypted: Mapped[str | None] = mapped_column(Text)
    operator_last4: Mapped[str] = mapped_column(String(4), nullable=False, default="")
    segment_count: Mapped[int] = mapped_column(Integer, nullable=False, default=0)
    captured_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    duration_ms: Mapped[int] = mapped_column(Integer, nullable=False)
    stop_reason: Mapped[str] = mapped_column(String(32), nullable=False)
    source_size_bytes: Mapped[int] = mapped_column(BigInteger, nullable=False)
    source_sha256: Mapped[str] = mapped_column(String(64), nullable=False)
    status: Mapped[str] = mapped_column(String(24), nullable=False, default="uploading", index=True)
    media_key_version: Mapped[int | None] = mapped_column(Integer)
    media_storage_name: Mapped[str | None] = mapped_column(String(80), unique=True)
    media_size_bytes: Mapped[int | None] = mapped_column(BigInteger)
    media_sha256: Mapped[str | None] = mapped_column(String(64))
    listened_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    deleted_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    deletion_reason: Mapped[str | None] = mapped_column(String(32))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
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


class AuditLog(Base):
    __tablename__ = "audit_logs"

    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    actor: Mapped[str] = mapped_column(String(320), nullable=False)
    action: Mapped[str] = mapped_column(String(120), nullable=False)
    target: Mapped[str] = mapped_column(String(160), nullable=False)
    details: Mapped[dict[str, Any]] = mapped_column(JSON, default=dict)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
