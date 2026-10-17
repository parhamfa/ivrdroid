"""Run against an explicitly supplied disposable PostgreSQL database."""
import os
from concurrent.futures import ThreadPoolExecutor
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, text, select
from sqlalchemy.engine import make_url

from app.main import create_app
from app.models import CallEventReceipt, CallRecord
from test_call_recovery import fixture, event
from test_ntfy import enable_ntfy


@pytest.mark.skipif(not os.environ.get("IVRDROID_TEST_POSTGRES_URL"), reason="Disposable PostgreSQL URL required")
def test_concurrent_parent_uploads_late_events_and_retries_preserve_evidence_once(settings, monkeypatch):
    url = make_url(os.environ["IVRDROID_TEST_POSTGRES_URL"])
    schema = "sept19_" + uuid4().hex
    engine = create_engine(url)
    with engine.begin() as connection:
        connection.execute(text(f'CREATE SCHEMA "{schema}"'))
    isolated = url.update_query_dict({"options": f"-csearch_path={schema}"})
    application = create_app(settings.model_copy(update={"database_url": isolated.render_as_string(hide_password=False)}))
    sent = []
    monkeypatch.setattr("app.ntfy.deliver_notification", sent.append)
    try:
        with TestClient(application) as client:
            enable_ntfy(client)
            headers, call = fixture(client)
            call.update(result="REMOTE_HANGUP", duration_seconds=30)
            def request(index):
                if index % 3 == 0:
                    return client.post("/api/device/v1/events:batch", headers=headers, json={"calls": [call]})
                document = event(at=f"2026-09-19T10:00:{10 + index % 4:02d}Z")
                return client.post("/api/device/v1/call-events:batch", headers=headers,
                    json={"calls": [{"call_id": call["call_id"], "events": [document]}]})
            with ThreadPoolExecutor(max_workers=8) as pool:
                responses = list(pool.map(request, range(48)))
            assert all(response.status_code == 200 for response in responses), [r.text for r in responses if r.status_code != 200]
            with application.state.database.session() as session:
                assert len(session.get(CallRecord, call["call_id"]).events) == 4
                assert len(session.scalars(select(CallEventReceipt)).all()) == 4
            assert len(sent) == 4
            assert all(item.title == "External call failed" for item in sent)
    finally:
        application.state.database.engine.dispose()
        with engine.begin() as connection:
            connection.execute(text(f'DROP SCHEMA "{schema}" CASCADE'))
        engine.dispose()
