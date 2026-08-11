"""Create the IVRdroid control-plane schema.

Revision ID: 0001_control_plane
Revises:
"""

from alembic import op
import sqlalchemy as sa


revision = "0001_control_plane"
down_revision = None
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "drafts",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("document_encrypted", sa.Text(), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("updated_by", sa.String(length=320), nullable=False),
    )
    op.create_table(
        "prompts",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("name", sa.String(length=120), nullable=False),
        sa.Column("version", sa.Integer(), nullable=False),
        sa.Column("content_hash", sa.String(length=64), nullable=False),
        sa.Column("size_bytes", sa.BigInteger(), nullable=False),
        sa.Column("duration_ms", sa.Integer(), nullable=False),
        sa.Column("storage_name", sa.String(length=80), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_by", sa.String(length=320), nullable=False),
        sa.UniqueConstraint("content_hash"),
        sa.UniqueConstraint("storage_name"),
    )
    op.create_index("ix_prompts_content_hash", "prompts", ["content_hash"])
    op.create_table(
        "revisions",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("manifest_encrypted", sa.Text(), nullable=False),
        sa.Column("manifest_sha256", sa.String(length=64), nullable=False),
        sa.Column("signature_b64", sa.Text(), nullable=False),
        sa.Column("source_revision_id", sa.Integer(), sa.ForeignKey("revisions.id")),
        sa.Column("published_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("published_by", sa.String(length=320), nullable=False),
        sa.UniqueConstraint("manifest_sha256"),
    )
    op.create_table(
        "devices",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("display_name", sa.String(length=120), nullable=False),
        sa.Column("token_hash", sa.String(length=64), nullable=False),
        sa.Column("app_version", sa.String(length=80), nullable=False),
        sa.Column("helper_version", sa.String(length=80), nullable=False),
        sa.Column("status", sa.JSON(), nullable=False),
        sa.Column("desired_revision_id", sa.Integer(), sa.ForeignKey("revisions.id")),
        sa.Column("active_revision_id", sa.Integer(), sa.ForeignKey("revisions.id")),
        sa.Column("last_seen_at", sa.DateTime(timezone=True)),
        sa.Column("enrolled_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("revoked_at", sa.DateTime(timezone=True)),
        sa.UniqueConstraint("token_hash"),
    )
    op.create_table(
        "pairing_codes",
        sa.Column("id", sa.String(length=36), primary_key=True),
        sa.Column("code_digest", sa.String(length=64), nullable=False),
        sa.Column("display_name", sa.String(length=120), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("failures", sa.Integer(), nullable=False),
        sa.Column("used_at", sa.DateTime(timezone=True)),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_by", sa.String(length=320), nullable=False),
        sa.UniqueConstraint("code_digest"),
    )
    op.create_table(
        "enrollment_attempts",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("source_ip", sa.String(length=64), nullable=False),
        sa.Column("succeeded", sa.Boolean(), nullable=False),
        sa.Column("attempted_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_index("ix_enrollment_attempts_source_ip", "enrollment_attempts", ["source_ip"])
    op.create_table(
        "call_records",
        sa.Column("id", sa.String(length=80), primary_key=True),
        sa.Column("device_id", sa.String(length=36), sa.ForeignKey("devices.id"), nullable=False),
        sa.Column("started_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("caller_encrypted", sa.Text()),
        sa.Column("caller_last4", sa.String(length=4), nullable=False),
        sa.Column("policy_decision", sa.String(length=64), nullable=False),
        sa.Column("revision_id", sa.Integer()),
        sa.Column("menu_path", sa.JSON(), nullable=False),
        sa.Column("result", sa.String(length=64), nullable=False),
        sa.Column("duration_seconds", sa.Integer(), nullable=False),
        sa.Column("events", sa.JSON(), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.create_table(
        "audit_logs",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("actor", sa.String(length=320), nullable=False),
        sa.Column("action", sa.String(length=120), nullable=False),
        sa.Column("target", sa.String(length=160), nullable=False),
        sa.Column("details", sa.JSON(), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
    )


def downgrade() -> None:
    op.drop_table("audit_logs")
    op.drop_table("call_records")
    op.drop_index("ix_enrollment_attempts_source_ip", table_name="enrollment_attempts")
    op.drop_table("enrollment_attempts")
    op.drop_table("pairing_codes")
    op.drop_table("devices")
    op.drop_table("revisions")
    op.drop_index("ix_prompts_content_hash", table_name="prompts")
    op.drop_table("prompts")
    op.drop_table("drafts")
