"""Add optional SMTP email notifications beside ntfy; additive and rollback-safe."""
from alembic import op
import sqlalchemy as sa

revision = "0011_email_settings"
down_revision = "0010_call_recovery"
branch_labels = None
depends_on = None


def upgrade() -> None:
    # Email routing stays out of ntfy_settings.events, which 0.10.3 validates with extra keys forbidden.
    op.add_column("ntfy_settings", sa.Column("email_events", sa.JSON(), nullable=True))
    op.create_table(
        "email_settings",
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("enabled", sa.Boolean(), nullable=False, server_default=sa.false()),
        sa.Column("smtp_host", sa.String(253), nullable=False, server_default="smtp.mail.me.com"),
        sa.Column("smtp_port", sa.Integer(), nullable=False, server_default=sa.text("587")),
        sa.Column("smtp_security", sa.String(16), nullable=False, server_default="starttls"),
        sa.Column("smtp_username", sa.String(254), nullable=False, server_default=""),
        sa.Column("smtp_password_encrypted", sa.Text(), nullable=True),
        sa.Column("from_address", sa.String(254), nullable=False, server_default=""),
        sa.Column("from_name", sa.String(80), nullable=False, server_default="IVRdroid"),
        sa.Column("recipients", sa.JSON(), nullable=False),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()),
        sa.Column("updated_by", sa.String(320), nullable=False),
        sa.CheckConstraint("id = 1", name="email_settings_singleton"),
        sa.CheckConstraint("smtp_security IN ('starttls', 'ssl')", name="email_settings_security"),
    )


def downgrade() -> None:
    op.drop_table("email_settings")
    with op.batch_alter_table("ntfy_settings") as batch:
        batch.drop_column("email_events")
