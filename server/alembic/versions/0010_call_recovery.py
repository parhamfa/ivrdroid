"""Disconnect evidence and independent call-event receipts; additive rollback-safe state."""
from alembic import op
import sqlalchemy as sa

revision = "0010_call_recovery"
down_revision = "0009_display_settings"
branch_labels = None
depends_on = None


def upgrade():
    op.add_column("continuous_recordings", sa.Column("validation_error", sa.String(500)))
    op.add_column("call_records", sa.Column("ended_at", sa.DateTime(timezone=True)))
    op.add_column("call_records", sa.Column("cleanup_status", sa.String(32)))
    # Existing terminal reports already generated their alerts. Never replay them.
    op.add_column("call_records", sa.Column("terminal_notification_scheduled", sa.Boolean(), nullable=False, server_default=sa.true()))
    op.execute("UPDATE call_records SET terminal_notification_scheduled = false WHERE result = 'IN_PROGRESS'")
    op.create_table("call_event_receipts",
        sa.Column("device_id", sa.String(36), sa.ForeignKey("devices.id"), primary_key=True),
        sa.Column("call_id", sa.String(80), primary_key=True),
        sa.Column("identity", sa.String(64), primary_key=True),
        sa.Column("document", sa.JSON(), nullable=False),
        sa.Column("notification_scheduled", sa.Boolean(), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False))
    op.create_table("call_history_corrections",
        sa.Column("call_id", sa.String(80), sa.ForeignKey("call_records.id"), primary_key=True),
        sa.Column("evidence_sha256", sa.String(64), nullable=False),
        sa.Column("original", sa.JSON(), nullable=False),
        sa.Column("applied", sa.JSON(), nullable=False),
        sa.Column("corrected_at", sa.DateTime(timezone=True), nullable=False))


def downgrade():
    raise RuntimeError("Retain call evidence and receipt tables during application rollback.")
