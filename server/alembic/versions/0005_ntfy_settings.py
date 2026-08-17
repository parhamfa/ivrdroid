"""Add owner-configured ntfy notification settings.

Revision ID: 0005_ntfy_settings
Revises: 0004_external_conversations
"""

from alembic import op
import sqlalchemy as sa


revision = "0005_ntfy_settings"
down_revision = "0004_external_conversations"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "ntfy_settings",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("enabled", sa.Boolean(), nullable=False, server_default=sa.false()),
        sa.Column("server_url", sa.String(length=200), nullable=False, server_default="https://ntfy.sh"),
        sa.Column("topic", sa.String(length=64), nullable=False, server_default=""),
        sa.Column("token_encrypted", sa.Text(), nullable=True),
        sa.Column("events", sa.JSON(), nullable=False),
        sa.Column("last_offline_notified_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("last_storage_notified_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_by", sa.String(length=320), nullable=False),
    )


def downgrade() -> None:
    op.drop_table("ntfy_settings")
