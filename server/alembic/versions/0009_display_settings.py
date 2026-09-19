"""Add shared dashboard timezone and calendar preferences."""
from alembic import op
import sqlalchemy as sa

revision = "0009_display_settings"
down_revision = "0008_continuous_recordings"
branch_labels = None
depends_on = None


def upgrade() -> None:
    table = op.create_table(
        "display_settings",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("timezone", sa.String(80), nullable=False, server_default="Asia/Tehran"),
        sa.Column("date_calendar", sa.String(16), nullable=False, server_default="persian"),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_by", sa.String(320), nullable=False),
        sa.CheckConstraint("id = 1", name="display_settings_singleton"),
        sa.CheckConstraint("date_calendar IN ('gregorian', 'persian')", name="display_settings_calendar"),
    )
    op.execute(table.insert().values(id=1, updated_by="system"))


def downgrade() -> None:
    op.drop_table("display_settings")
