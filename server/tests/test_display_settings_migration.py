from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, inspect, text

from app.config import load_settings
from app.database import Base


def test_display_migration_preserves_existing_data_and_saved_preferences(tmp_path, monkeypatch):
    url = f"sqlite:///{tmp_path / 'migration.db'}"
    monkeypatch.setenv("IVRDROID_DATABASE_URL", url)
    load_settings.cache_clear()
    engine = create_engine(url)
    # Model the existing 0008 schema without replaying older PostgreSQL-only ALTER statements.
    Base.metadata.create_all(engine, tables=[table for table in Base.metadata.sorted_tables if table.name != "display_settings"])
    with engine.begin() as connection:
        connection.execute(text("INSERT INTO audit_logs (actor, action, target, details, created_at) VALUES ('test', 'existing', 'existing', '{}', CURRENT_TIMESTAMP)"))
    config = Config("alembic.ini")
    try:
        command.stamp(config, "0008_continuous_recordings")
        command.upgrade(config, "0009_display_settings")
        with engine.begin() as connection:
            assert connection.execute(text("SELECT timezone, date_calendar FROM display_settings")).one() == ("Asia/Tehran", "persian")
            connection.execute(text("UPDATE display_settings SET timezone = 'UTC', date_calendar = 'gregorian' WHERE id = 1"))
        command.upgrade(config, "0009_display_settings")
        with engine.connect() as connection:
            assert connection.execute(text("SELECT timezone, date_calendar FROM display_settings")).one() == ("UTC", "gregorian")
            assert connection.execute(text("SELECT action FROM audit_logs")).scalar_one() == "existing"
        command.downgrade(config, "0008_continuous_recordings")
        assert "display_settings" not in inspect(engine).get_table_names()
        with engine.connect() as connection:
            assert connection.execute(text("SELECT action FROM audit_logs")).scalar_one() == "existing"
    finally:
        engine.dispose()
        load_settings.cache_clear()
