"""Add V3 voicemail upload, encrypted media, and retention tables.

Revision ID: 0003_voicemail_recordings
Revises: 0002_flow_runtime_v2
"""

from alembic import op
import sqlalchemy as sa


revision = "0003_voicemail_recordings"
down_revision = "0002_flow_runtime_v2"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "recording_retention_policies",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("mode", sa.String(length=16), nullable=False, server_default="automatic"),
        sa.Column("days", sa.Integer(), nullable=False, server_default="30"),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_by", sa.String(length=320), nullable=False),
        sa.CheckConstraint("mode IN ('automatic', 'manual')", name="ck_recording_retention_mode"),
        sa.CheckConstraint("days BETWEEN 1 AND 365", name="ck_recording_retention_days"),
    )
    op.create_table(
        "recordings",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("device_id", sa.String(length=36), sa.ForeignKey("devices.id"), nullable=False),
        sa.Column("call_id", sa.String(length=80), sa.ForeignKey("call_records.id"), nullable=False),
        sa.Column("revision_id", sa.Integer(), sa.ForeignKey("revisions.id"), nullable=False),
        sa.Column("block_id", sa.String(length=36), nullable=False),
        sa.Column("sequence", sa.Integer(), nullable=False),
        sa.Column("captured_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("duration_ms", sa.Integer(), nullable=False),
        sa.Column("stop_reason", sa.String(length=32), nullable=False),
        sa.Column("source_size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("source_sha256", sa.String(length=64), nullable=False),
        sa.Column("status", sa.String(length=24), nullable=False, server_default="uploading"),
        sa.Column("media_key_version", sa.Integer()),
        sa.Column("media_storage_name", sa.String(length=80), unique=True),
        sa.Column("media_size_bytes", sa.BigInteger()),
        sa.Column("media_sha256", sa.String(length=64)),
        sa.Column("listened_at", sa.DateTime(timezone=True)),
        sa.Column("deleted_at", sa.DateTime(timezone=True)),
        sa.Column("deletion_reason", sa.String(length=32)),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.UniqueConstraint("device_id", "call_id", "sequence", name="uq_recording_call_sequence"),
    )
    op.create_index("ix_recordings_device_id", "recordings", ["device_id"])
    op.create_index("ix_recordings_call_id", "recordings", ["call_id"])
    op.create_index("ix_recordings_status", "recordings", ["status"])
    op.create_table(
        "recording_uploads",
        sa.Column(
            "recording_id",
            sa.String(length=36),
            sa.ForeignKey("recordings.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column("expected_size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("source_sha256", sa.String(length=64), nullable=False),
        sa.Column("upload_offset", sa.BigInteger(), nullable=False, server_default="0"),
        sa.Column("storage_name", sa.String(length=80), nullable=False, unique=True),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
    )
    op.create_index("ix_recording_uploads_updated_at", "recording_uploads", ["updated_at"])


def downgrade() -> None:
    op.drop_index("ix_recording_uploads_updated_at", table_name="recording_uploads")
    op.drop_table("recording_uploads")
    op.drop_index("ix_recordings_status", table_name="recordings")
    op.drop_index("ix_recordings_call_id", table_name="recordings")
    op.drop_index("ix_recordings_device_id", table_name="recordings")
    op.drop_table("recordings")
    op.drop_table("recording_retention_policies")
