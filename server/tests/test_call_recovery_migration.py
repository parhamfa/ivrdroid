"""Apply the real migration chain in a disposable PostgreSQL schema."""
import os
from uuid import uuid4

import pytest
from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, text
from sqlalchemy.engine import make_url

from app.config import load_settings


@pytest.mark.skipif(not os.environ.get("IVRDROID_TEST_POSTGRES_URL"), reason="Disposable PostgreSQL URL required")
def test_additive_upgrade_preserves_preferences_history_and_rollback_state(monkeypatch):
    url = make_url(os.environ["IVRDROID_TEST_POSTGRES_URL"])
    schema = "sept19_migration_" + uuid4().hex
    engine = create_engine(url)
    with engine.begin() as connection:
        connection.execute(text(f'CREATE SCHEMA "{schema}"'))
    scoped = url.update_query_dict({"options": f"-csearch_path={schema}"})
    monkeypatch.setenv("IVRDROID_DATABASE_URL", scoped.render_as_string(hide_password=False))
    load_settings.cache_clear()
    scoped_engine = create_engine(scoped)
    config = Config("alembic.ini")
    try:
        command.upgrade(config, "0009_display_settings")
        with scoped_engine.begin() as connection:
            connection.execute(text("UPDATE display_settings SET timezone='Europe/Sofia', date_calendar='gregorian'"))
            connection.execute(text("INSERT INTO devices (id, display_name, token_hash, app_version, helper_version, status, enrolled_at) VALUES ('migration-device', 'test', 'test-hash', '0.10.1', '0.10.1', '{}', now())"))
            for identity, outcome in [("finished", "REMOTE_HANGUP"), ("active", "IN_PROGRESS")]:
                connection.execute(text("INSERT INTO call_records (id, device_id, started_at, caller_last4, policy_decision, menu_path, result, duration_seconds, events, received_at) VALUES (:id, 'migration-device', now(), '', 'IVR_HANDLED', '[]', :outcome, 0, '[]', now())"), {"id": identity, "outcome": outcome})
        command.upgrade(config, "head")
        command.upgrade(config, "head")
        with scoped_engine.connect() as connection:
            assert connection.execute(text("SELECT version_num FROM alembic_version")).scalar_one() == "0010_call_recovery"
            assert connection.execute(text("SELECT timezone, date_calendar FROM display_settings")).one() == ("Europe/Sofia", "gregorian")
            assert dict(connection.execute(text("SELECT id, terminal_notification_scheduled FROM call_records")).all()) == {"finished": True, "active": False}
            assert connection.execute(text("SELECT count(*) FROM call_event_receipts")).scalar_one() == 0
        with pytest.raises(RuntimeError, match="Retain call evidence"):
            command.downgrade(config, "0009_display_settings")
        with scoped_engine.connect() as connection:
            assert connection.execute(text("SELECT count(*) FROM call_records")).scalar_one() == 2
            assert connection.execute(text("SELECT version_num FROM alembic_version")).scalar_one() == "0010_call_recovery"
    finally:
        scoped_engine.dispose()
        with engine.begin() as connection:
            connection.execute(text(f'DROP SCHEMA "{schema}" CASCADE'))
        engine.dispose()
        load_settings.cache_clear()
