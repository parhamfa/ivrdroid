"""Continuous PCM receipts and durable processing queue; retain all legacy data."""
from alembic import op
import sqlalchemy as sa

revision = "0008_continuous_recordings"
down_revision = "0007_call_safety"
branch_labels = None
depends_on = None


def upgrade():
    op.add_column("recordings", sa.Column("source_format", sa.String(32), nullable=False, server_default="legacy_wav"))
    op.add_column("recordings", sa.Column("partial", sa.Boolean(), nullable=False, server_default=sa.false()))
    op.add_column("recordings", sa.Column("processing_error", sa.String(500), nullable=True))
    op.create_table("continuous_recordings",
        sa.Column("recording_id", sa.String(36), sa.ForeignKey("recordings.id", ondelete="CASCADE"), primary_key=True),
        sa.Column("manifest", sa.JSON(), nullable=False),
        sa.Column("state", sa.String(24), nullable=False),
        sa.Column("attempts", sa.Integer(), nullable=False),
        sa.Column("retry_at", sa.DateTime(timezone=True)),
        sa.Column("accepted_at", sa.DateTime(timezone=True)),
        sa.Column("operation_duration_ms", sa.BigInteger()),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False))
    op.create_index("ix_continuous_recordings_state", "continuous_recordings", ["state"])


def downgrade():
    raise RuntimeError("Keep continuous recording tables and media during service rollback.")
