"""Add owned-tree draft versioning and legacy draft archive.

Revision ID: 0002_flow_runtime_v2
Revises: 0001_control_plane
"""

from alembic import op
import sqlalchemy as sa


revision = "0002_flow_runtime_v2"
down_revision = "0001_control_plane"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.create_table(
        "legacy_draft_archives",
        sa.Column("id", sa.Integer(), primary_key=True, autoincrement=True),
        sa.Column("schema_version", sa.Integer(), nullable=False, server_default="1"),
        sa.Column("document_encrypted", sa.Text(), nullable=False),
        sa.Column("archived_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("archived_by", sa.String(length=320), nullable=False),
    )
    op.execute(
        """
        INSERT INTO legacy_draft_archives
            (schema_version, document_encrypted, archived_at, archived_by)
        SELECT 1, document_encrypted, CURRENT_TIMESTAMP, updated_by
        FROM drafts
        """,
    )
    op.add_column(
        "drafts",
        sa.Column("edit_version", sa.Integer(), nullable=False, server_default="1"),
    )
    op.add_column(
        "revisions",
        sa.Column("schema_version", sa.Integer(), nullable=False, server_default="1"),
    )


def downgrade() -> None:
    op.drop_column("revisions", "schema_version")
    op.drop_column("drafts", "edit_version")
    op.drop_table("legacy_draft_archives")
