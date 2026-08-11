"""Add V4 external-call conversation recording metadata and segments.

Revision ID: 0004_external_conversations
Revises: 0003_voicemail_recordings
"""

from alembic import op
import sqlalchemy as sa


revision = "0004_external_conversations"
down_revision = "0003_voicemail_recordings"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "recordings",
        sa.Column("kind", sa.String(length=24), nullable=False, server_default="voicemail"),
    )
    op.add_column("recordings", sa.Column("operator_encrypted", sa.Text(), nullable=True))
    op.add_column(
        "recordings",
        sa.Column("operator_last4", sa.String(length=4), nullable=False, server_default=""),
    )
    op.add_column(
        "recordings",
        sa.Column("segment_count", sa.Integer(), nullable=False, server_default="0"),
    )
    op.create_index("ix_recordings_kind", "recordings", ["kind"])
    op.create_table(
        "conversation_recording_segments",
        sa.Column(
            "recording_id",
            sa.String(length=36),
            sa.ForeignKey("recordings.id", ondelete="CASCADE"),
            primary_key=True,
        ),
        sa.Column("segment_index", sa.Integer(), primary_key=True),
        sa.Column("captured_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("duration_ms", sa.Integer(), nullable=False),
        sa.Column("stop_reason", sa.String(length=32), nullable=False),
        sa.Column("partial", sa.Boolean(), nullable=False, server_default=sa.false()),
        sa.Column("expected_size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("source_sha256", sa.String(length=64), nullable=False),
        sa.Column("upload_offset", sa.BigInteger(), nullable=False, server_default="0"),
        sa.Column("storage_name", sa.String(length=80), nullable=False, unique=True),
        sa.Column("status", sa.String(length=24), nullable=False, server_default="uploading"),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.UniqueConstraint(
            "recording_id",
            "segment_index",
            name="uq_conversation_recording_segment_index",
        ),
    )
    op.create_index(
        "ix_conversation_recording_segments_status",
        "conversation_recording_segments",
        ["status"],
    )
    op.create_index(
        "ix_conversation_recording_segments_updated_at",
        "conversation_recording_segments",
        ["updated_at"],
    )


def downgrade() -> None:
    op.drop_index(
        "ix_conversation_recording_segments_updated_at",
        table_name="conversation_recording_segments",
    )
    op.drop_index(
        "ix_conversation_recording_segments_status",
        table_name="conversation_recording_segments",
    )
    op.drop_table("conversation_recording_segments")
    op.drop_index("ix_recordings_kind", table_name="recordings")
    op.drop_column("recordings", "segment_count")
    op.drop_column("recordings", "operator_last4")
    op.drop_column("recordings", "operator_encrypted")
    op.drop_column("recordings", "kind")
