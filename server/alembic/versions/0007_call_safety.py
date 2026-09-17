"""Signed call lifetime policies and durable acknowledgments. Additive only."""
from alembic import op
import sqlalchemy as sa

revision = "0007_call_safety"
down_revision = "0006_session_audit"
branch_labels = None
depends_on = None


def upgrade():
    op.create_table("call_safety_policies",
        sa.Column("version", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("maximum_call_duration_seconds", sa.Integer(), nullable=False),
        sa.Column("created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("created_by", sa.String(320), nullable=False),
        sa.CheckConstraint("maximum_call_duration_seconds BETWEEN 60 AND 86400", name="ck_call_safety_duration"))
    op.create_table("device_call_safety_acknowledgements",
        sa.Column("device_id", sa.String(36), sa.ForeignKey("devices.id"), primary_key=True),
        sa.Column("policy_version", sa.Integer(), primary_key=True),
        sa.Column("maximum_call_duration_seconds", sa.Integer(), nullable=False),
        sa.Column("applied_at", sa.DateTime(timezone=True), nullable=False))


def downgrade():
    raise RuntimeError("Roll back services with additive tables intact; do not erase policy acknowledgments.")
