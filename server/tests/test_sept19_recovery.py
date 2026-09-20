import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import pytest
from sqlalchemy import select

from app import sept19_recovery as repair
from app.models import CallEventReceipt, CallHistoryCorrection, CallRecord, Recording
from test_capture_receipts import manifest
from test_session_audit import context


def test_historical_repair_backs_up_source_and_original_values_and_deduplicates_without_alerts(client, monkeypatch, tmp_path):
    ctx = context(client, duration_ms=100)
    pcm = b"\0\1\0\2" * 4800
    call_id = ctx["call_id"]
    url = "/api/device/v1/continuous-recordings"
    body = manifest(ctx, pcm)
    assert client.post(url, headers=ctx["headers"], json=body).status_code == 201
    assert client.put(url + "/" + ctx["recording_id"] + "/content", headers={**ctx["headers"], "Upload-Offset": "0"}, content=pcm).status_code == 200
    with client.app.state.database.session() as session:
        call = session.get(CallRecord, call_id)
        call.result = "RECOVERED_AND_ENDED"
        saved = repair.row_document(call)
        session.commit()
    end = datetime.fromisoformat(saved["started_at"]).replace(tzinfo=timezone.utc).isoformat()
    proof = {"calls": [{**saved, "started_at": end}], "events": []}
    evidence = tmp_path / "evidence"; evidence.mkdir(); (evidence / "proof.txt").write_text("fixture evidence")
    monkeypatch.setattr(repair, "CORRECTIONS", {call_id: (end, "recovered")})
    monkeypatch.setattr(repair, "SOURCES", {call_id: (len(pcm), hashlib.sha256(pcm).hexdigest())})
    monkeypatch.setattr(repair, "evidence_bundle", lambda _: proof)
    monkeypatch.setattr("app.ntfy.deliver_notification", lambda _: pytest.fail("Historical repair must not alert"))
    settings = client.app.state.settings
    dry = repair.run(settings, evidence)
    assert not dry["applied"] and dry["requeue_verified_sources"] == [ctx["recording_id"]]
    with client.app.state.database.session() as session:
        assert session.get(CallRecord, call_id).result == "RECOVERED_AND_ENDED"
        assert session.get(CallHistoryCorrection, call_id) is None
    backup = tmp_path / "backup"
    assert repair.run(settings, evidence, apply=True, backup_dir=backup)["applied"]
    assert json.loads((backup / "original-rows.json").read_text())["call_records"][0]["result"] == "RECOVERED_AND_ENDED"
    for name, expected in json.loads((backup / "SHA256.json").read_text()).items():
        assert hashlib.sha256((backup / name).read_bytes()).hexdigest() == expected
    with client.app.state.database.session() as session:
        assert session.get(CallRecord, call_id).result == "REMOTE_HANGUP"
        correction = session.get(CallHistoryCorrection, call_id)
        assert correction.original["result"] == "RECOVERED_AND_ENDED"
        assert correction.evidence_sha256 == repair.EVIDENCE_SHA256
        assert session.get(CallRecord, call_id).terminal_notification_scheduled
        assert all(row.notification_scheduled for row in session.scalars(select(CallEventReceipt)))
    assert repair.run(settings, evidence)["corrections"] == {}
    monkeypatch.setattr(repair, "SOURCES", {call_id: (len(pcm), "0" * 64)})
    with pytest.raises(ValueError, match="source"):
        repair.run(settings, evidence)


def test_evidence_index_must_match_the_investigated_bundle(tmp_path):
    (tmp_path / "SHA256SUMS").write_text("forged\n")
    with pytest.raises(ValueError, match="index differs"):
        repair.evidence_bundle(tmp_path)
