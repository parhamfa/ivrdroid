import pytest
import sqlalchemy as sa
from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, inspect, text
from sqlalchemy.exc import IntegrityError

from app.config import load_settings
from app.database import Base


def test_email_migration_is_additive_and_reversible(tmp_path, monkeypatch):
    url = f"sqlite:///{tmp_path / 'migration.db'}"
    monkeypatch.setenv("IVRDROID_DATABASE_URL", url)
    load_settings.cache_clear()
    engine = create_engine(url)
    # Today's metadata already has both 0011 changes, so build ntfy_settings in its 0010 shape by hand.
    Base.metadata.create_all(
        engine,
        tables=[table for table in Base.metadata.sorted_tables if table.name not in {"email_settings", "ntfy_settings"}],
    )
    sa.Table(
        "ntfy_settings",
        sa.MetaData(),
        sa.Column("id", sa.Integer(), primary_key=True),
        sa.Column("enabled", sa.Boolean(), nullable=False),
        sa.Column("server_url", sa.String(200), nullable=False),
        sa.Column("topic", sa.String(64), nullable=False),
        sa.Column("token_encrypted", sa.Text()),
        sa.Column("events", sa.JSON(), nullable=False),
        sa.Column("last_offline_notified_at", sa.DateTime(timezone=True)),
        sa.Column("last_storage_notified_at", sa.DateTime(timezone=True)),
        sa.Column("updated_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("updated_by", sa.String(320), nullable=False),
    ).create(engine)
    with engine.begin() as connection:
        connection.execute(text("INSERT INTO ntfy_settings (id, enabled, server_url, topic, events, updated_at, updated_by) VALUES (1, 1, 'https://ntfy.sh', 'existing-topic', '{}', CURRENT_TIMESTAMP, 'owner@example.com')"))
    config = Config("alembic.ini")
    try:
        command.stamp(config, "0010_call_recovery")
        command.upgrade(config, "0011_email_settings")
        with engine.begin() as connection:
            assert connection.execute(text("SELECT topic, email_events FROM ntfy_settings")).one() == ("existing-topic", None)
            connection.execute(text("INSERT INTO email_settings (id, recipients, updated_by) VALUES (1, '[]', 'system')"))
            assert connection.execute(text("SELECT enabled, smtp_host, smtp_port, smtp_security, from_name FROM email_settings")).one() == (False, "smtp.mail.me.com", 587, "starttls", "IVRdroid")
        with pytest.raises(IntegrityError), engine.begin() as connection:
            connection.execute(text("UPDATE email_settings SET smtp_security = 'plain' WHERE id = 1"))
        command.downgrade(config, "0010_call_recovery")
        inspector = inspect(engine)
        assert "email_settings" not in inspector.get_table_names()
        assert "email_events" not in {column["name"] for column in inspector.get_columns("ntfy_settings")}
        with engine.connect() as connection:
            assert connection.execute(text("SELECT topic FROM ntfy_settings")).scalar_one() == "existing-topic"
    finally:
        engine.dispose()
        load_settings.cache_clear()
