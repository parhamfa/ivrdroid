"""Independent session audit policy and call recording metadata.

Revision ID: 0006_session_audit
Revises: 0005_ntfy_settings
"""
from alembic import op
import sqlalchemy as sa

revision = "0006_session_audit"
down_revision = "0005_ntfy_settings"
branch_labels = None
depends_on = None


def upgrade():
    op.create_table(
        "session_audit_policies",
        sa.Column("version", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("enabled", sa.Boolean(), nullable=False),
        sa.Column("local_quota_bytes", sa.BigInteger(), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_by", sa.String(320), nullable=False),
    )
    op.create_table(
        "device_audit_policy_acknowledgements",
        sa.Column("device_id", sa.String(36), sa.ForeignKey("devices.id"), primary_key=True),
        sa.Column("policy_version", sa.Integer(), sa.ForeignKey("session_audit_policies.version"), primary_key=True),
        sa.Column("applied_at", sa.DateTime(timezone=True), nullable=False),
    )
    op.add_column("call_records", sa.Column("session_audit", sa.JSON(), nullable=True))
    op.add_column("recordings", sa.Column("audit_metadata", sa.JSON(), nullable=True))
    op.alter_column("recordings", "revision_id", existing_type=sa.Integer(), nullable=True)
    op.alter_column("recordings", "block_id", existing_type=sa.String(36), nullable=True)
    op.create_index("uq_session_audit_call", "recordings", ["device_id", "call_id"], unique=True,
                    postgresql_where=sa.text("kind = 'session_audit'"),
                    sqlite_where=sa.text("kind = 'session_audit'"))
    op.create_check_constraint("ck_recording_identity", "recordings",
        "(kind = 'session_audit' AND block_id IS NULL) OR "
        "(kind IN ('voicemail', 'conversation') AND block_id IS NOT NULL AND revision_id IS NOT NULL)")


def downgrade():
    raise RuntimeError("Restore compatible services without removing session audit data.")
