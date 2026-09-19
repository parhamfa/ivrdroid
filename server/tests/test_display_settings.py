from fastapi.testclient import TestClient
import pytest
from sqlalchemy import select

from app.auth import require_admin
from app.main import create_app
from app.models import AuditLog, DisplaySettings

URL = "/api/admin/v1/display-settings"
DEFAULTS = {"timezone": "Asia/Tehran", "date_calendar": "persian"}


def test_defaults_and_shared_persistence(client, settings):
    assert client.get(URL).json() == DEFAULTS
    draft = client.get("/api/admin/v4/draft").json()
    changed = {"timezone": "Europe/Sofia", "date_calendar": "gregorian"}
    response = client.put(URL, json=changed)
    assert response.status_code == 200
    assert response.json() == changed
    client.app.dependency_overrides[require_admin] = lambda: "another@example.com"
    try:
        assert client.get(URL).json() == changed
    finally:
        client.app.dependency_overrides.clear()
    assert client.get("/api/admin/v4/draft").json() == draft
    with TestClient(create_app(settings)) as restarted:
        assert restarted.get(URL).json() == changed
        assert restarted.put(URL, json=DEFAULTS).json() == DEFAULTS
    with client.app.state.database.session() as session:
        assert len(session.scalars(select(DisplaySettings)).all()) == 1
        events = session.scalars(select(AuditLog).where(AuditLog.action == "display.settings_updated").order_by(AuditLog.id)).all()
        assert len(events) == 2
        assert events[0].actor == "owner@example.com"
        assert events[0].details == changed


@pytest.mark.parametrize("patch", [
    {"timezone": ""}, {"timezone": "Mars/Olympus"}, {"timezone": "../UTC"},
    {"timezone": "/etc/passwd"}, {"timezone": " Asia/Tehran "}, {"timezone": None},
    {"date_calendar": "islamic"}, {"date_calendar": None}, {"unexpected": True},
])
def test_invalid_settings_do_not_change_saved_values(client, patch):
    client.put(URL, json=DEFAULTS)
    assert client.put(URL, json={**DEFAULTS, **patch}).status_code == 422
    assert client.get(URL).json() == DEFAULTS
    with client.app.state.database.session() as session:
        assert len(session.scalars(select(AuditLog).where(AuditLog.action == "display.settings_updated")).all()) == 1


def test_authentication_and_same_origin_write(client, settings):
    # Switch after startup so the test can exercise production request guards without live credentials.
    settings.environment = "production"
    assert client.get(URL).status_code == 401
    assert client.put(URL, json=DEFAULTS).status_code == 401
    client.app.dependency_overrides[require_admin] = lambda: "signed-in@example.com"
    try:
        assert client.get(URL).status_code == 200
        assert client.put(URL, json=DEFAULTS).status_code == 403
        assert client.put(URL, json=DEFAULTS, headers={"Origin": "https://elsewhere.test"}).status_code == 403
        assert client.put(URL, json=DEFAULTS, headers={"Origin": settings.public_base_url}).status_code == 200
    finally:
        settings.environment = "test"
        client.app.dependency_overrides.clear()
