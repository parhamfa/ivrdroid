from __future__ import annotations

import json
import logging
import smtplib
import socket
import ssl

import pytest
from sqlalchemy import select

from app.email_notify import EmailPayload, describe_smtp_error
from app.models import Device, EmailSettings, NtfySettings, utcnow
from app.ntfy import call_notification_kwargs, dispatch_ntfy, evaluate_presence, evaluate_storage, notify_storage_full
from test_ntfy import add_device, enable_ntfy

EMAIL_URL = "/api/admin/v1/email-settings"
NTFY_URL = "/api/admin/v1/ntfy-settings"
USERNAME = "owner.appleid@icloud.com"
PASSWORD = "app-specific-secret"


def email_body(**changes) -> dict:
    return {
        "enabled": True,
        "smtp_host": "smtp.mail.me.com",
        "smtp_port": 587,
        "smtp_security": "starttls",
        "smtp_username": USERNAME,
        "smtp_password": PASSWORD,
        "from_address": "ivrdroid@icloud.com",
        "from_name": "IVRdroid",
        "recipients": ["alerts@example.com"],
        **changes,
    }


def enable_email(client, **changes) -> dict:
    response = client.put(EMAIL_URL, json=email_body(**changes))
    assert response.status_code == 200, response.text
    return response.json()


def current_events(client) -> dict:
    return client.get(NTFY_URL).json()["events"]


def save_events(client, events: dict, *, ntfy: bool) -> None:
    if ntfy:
        enable_ntfy(client, events=events)
        return
    response = client.put(
        NTFY_URL,
        json={"enabled": False, "server_url": "https://ntfy.sh", "topic": "", "events": events},
    )
    assert response.status_code == 200, response.text


def capture(monkeypatch) -> tuple[list, list]:
    pushed: list = []
    emailed: list = []
    monkeypatch.setattr("app.ntfy.deliver_notification", pushed.append)
    monkeypatch.setattr("app.email_notify.deliver_email", emailed.append)
    return pushed, emailed


def fake_smtp(monkeypatch, name: str = "SMTP", *, login_error=None, starttls_error=None) -> list[tuple]:
    calls: list[tuple] = []

    class FakeSMTP:
        def __init__(self, host, port, **kwargs):
            calls.append(("connect", host, port, kwargs))

        def ehlo(self):
            calls.append(("ehlo",))

        def starttls(self, *, context=None):
            calls.append(("starttls", context))
            if starttls_error is not None:
                raise starttls_error

        def login(self, username, password):
            calls.append(("login", username, password))
            if login_error is not None:
                raise login_error

        def send_message(self, message, *, from_addr=None, to_addrs=None):
            calls.append(("send", message, from_addr, to_addrs))
            return {}

        def quit(self):
            calls.append(("quit",))

        def close(self):
            calls.append(("close",))

    monkeypatch.setattr(smtplib, name, FakeSMTP)
    return calls


def test_email_settings_round_trip_keeps_password_server_side(client) -> None:
    created = client.get(EMAIL_URL)
    assert created.status_code == 200
    body = created.json()
    assert body["enabled"] is False
    assert (body["smtp_host"], body["smtp_port"], body["smtp_security"]) == ("smtp.mail.me.com", 587, "starttls")
    assert body["from_name"] == "IVRdroid"
    assert body["recipients"] == []
    assert body["password_configured"] is False
    assert "smtp_password" not in body

    saved = enable_email(client, recipients=[" Alerts@Example.COM ", "alerts@example.com", "second@example.com"])
    assert saved["password_configured"] is True
    assert saved["recipients"] == ["alerts@example.com", "second@example.com"]
    assert PASSWORD not in json.dumps(saved)
    with client.app.state.database.session() as session:
        stored = session.get(EmailSettings, 1).smtp_password_encrypted
    assert PASSWORD not in stored
    assert client.app.state.cipher.decrypt(stored) == PASSWORD

    assert enable_email(client, smtp_password="   ")["password_configured"] is True
    assert enable_email(client, smtp_password=None)["password_configured"] is True
    with client.app.state.database.session() as session:
        assert session.get(EmailSettings, 1).smtp_password_encrypted == stored

    cleared = enable_email(client, smtp_username="", smtp_password=None)
    assert cleared["password_configured"] is False
    with client.app.state.database.session() as session:
        assert session.get(EmailSettings, 1).smtp_password_encrypted is None
    missing = client.put(EMAIL_URL, json=email_body(smtp_password=None))
    assert missing.status_code == 400
    assert "password" in missing.json()["detail"]

    audit = [item for item in client.get("/api/admin/v1/audit").json() if item["action"] == "email.settings_updated"]
    assert len(audit) == 4
    assert {item["details"]["password_changed"] for item in audit} == {True, False}
    assert PASSWORD not in json.dumps(audit)
    assert USERNAME not in json.dumps(audit)


@pytest.mark.parametrize(
    "changes",
    [
        {"recipients": ["alerts@example.com\r\nBcc: attacker@example.com"]},
        {"recipients": ["Owner <owner@example.com>"]},
        {"recipients": [f"user{index}@example.com" for index in range(6)]},
        {"recipients": []},
        {"from_name": "IVRdroid\r\nBcc: attacker@example.com"},
        {"from_address": "owner@@icloud.com"},
        {"from_address": "owner..name@icloud.com"},
        {"smtp_security": "plain"},
        {"smtp_host": "host:587"},
        {"smtp_host": "https://smtp.mail.me.com"},
        {"smtp_port": 0},
        {"smtp_username": "owner appleid@icloud.com"},
    ],
)
def test_email_settings_reject_unsafe_values(client, changes) -> None:
    assert client.put(EMAIL_URL, json=email_body(**changes)).status_code == 422


def test_each_event_routes_to_push_email_or_both(client, monkeypatch) -> None:
    pushed, emailed = capture(monkeypatch)
    enable_email(client)
    events = current_events(client)
    routes = {
        "voicemail_ready": (True, False),
        "ivr_session_failed": (False, True),
        "revision_activation_failed": (True, True),
        "conversation_ready": (False, False),
    }
    for key, (push, email) in routes.items():
        events[key].update(enabled=push, email=email)
    save_events(client, events, ntfy=True)
    for key in routes:
        dispatch_ntfy(client.app, key, title=key, message=f"{key} happened", click_path="/calls")
    assert [item.title for item in pushed] == ["voicemail_ready", "revision_activation_failed"]
    assert [item.subject for item in emailed] == [
        "[IVRdroid] ivr_session_failed",
        "[IVRdroid] revision_activation_failed",
    ]
    assert emailed[0].body == "ivr_session_failed happened\n\nhttps://ivrdroid.test/calls\n"
    assert emailed[0].recipients == ("alerts@example.com",)
    assert emailed[0].password == PASSWORD
    assert emailed[0].local_hostname == "ivrdroid.test"
    assert PASSWORD not in repr(emailed[0])

    enable_email(client, enabled=False)
    dispatch_ntfy(client.app, "ivr_session_failed", title="ivr_session_failed", message="again")
    assert len(emailed) == 2


def test_external_call_subtoggles_and_masked_caller_apply_to_email(client, monkeypatch) -> None:
    pushed, emailed = capture(monkeypatch)
    enable_email(client)
    events = current_events(client)
    events["external_call_failed"].update(enabled=False, email=True, not_connected=False)
    save_events(client, events, ntfy=False)

    def report(call_id: str, status: str) -> None:
        kwargs = call_notification_kwargs(
            "external_call_failed",
            call_id=call_id,
            result="SESSION_COMPLETE",
            policy_decision="IVR_HANDLED",
            extra={"external_status": status},
        )
        dispatch_ntfy(client.app, "external_call_failed", caller_masked="+15-***-4567", **kwargs)

    report("call-external-1", "NOT_CONNECTED")
    assert emailed == []
    report("call-external-2", "SYSTEM_FAILURE")
    assert [item.subject for item in emailed] == ["[IVRdroid] External call failed"]
    assert emailed[0].body.startswith("The operator step ended with SYSTEM_FAILURE.\nCaller +15-***-4567\n\n")
    assert "/calls?call=call-external-2" in emailed[0].body

    events["external_call_failed"]["include_masked_caller"] = False
    save_events(client, events, ntfy=False)
    report("call-external-3", "SYSTEM_FAILURE")
    assert "4567" not in emailed[1].body
    assert pushed == []


def test_email_only_tablet_offline_and_recovery(client, monkeypatch) -> None:
    pushed, emailed = capture(monkeypatch)
    enable_email(client)
    events = current_events(client)
    events["tablet_offline"]["email"] = True
    save_events(client, events, ntfy=False)
    add_device(client, minutes_ago=16, call_state="idle")
    evaluate_presence(client.app)
    evaluate_presence(client.app)
    assert [item.subject for item in emailed] == ["[IVRdroid] Tablet offline"]
    with client.app.state.database.session() as session:
        session.scalar(select(Device)).last_seen_at = utcnow()
        session.commit()
    evaluate_presence(client.app)
    assert [item.subject for item in emailed] == ["[IVRdroid] Tablet offline", "[IVRdroid] Tablet recovered"]
    assert pushed == []


def test_email_only_storage_alert_is_deduplicated(client, monkeypatch) -> None:
    pushed, emailed = capture(monkeypatch)
    enable_email(client)
    events = current_events(client)
    events["storage_full"]["email"] = True
    save_events(client, events, ntfy=False)
    with client.app.state.database.session() as session:
        session.add(
            Device(
                display_name="SM-T585",
                token_hash="c" * 64,
                status={"voicemail_spool_bytes": 512 * 1024 * 1024, "conversation_spool_bytes": 0, "call_state": "idle"},
                last_seen_at=utcnow(),
            ),
        )
        session.commit()
    evaluate_storage(client.app)
    evaluate_storage(client.app)
    notify_storage_full(client.app)
    assert [item.subject for item in emailed] == ["[IVRdroid] Recording storage is full"]
    assert pushed == []


def test_email_flags_stay_out_of_rollback_visible_events(client) -> None:
    events = current_events(client)
    assert all(config["email"] is False for config in events.values())
    events["ivr_session_failed"]["email"] = True
    save_events(client, events, ntfy=True)
    assert current_events(client)["ivr_session_failed"]["email"] is True
    with client.app.state.database.session() as session:
        row = session.get(NtfySettings, 1)
        assert all("email" not in config for config in row.events.values())
        assert row.email_events["ivr_session_failed"] is True
        assert row.email_events["voicemail_ready"] is False


def test_ntfy_put_without_email_flags_keeps_email_routing(client) -> None:
    events = current_events(client)
    events["ivr_session_failed"]["email"] = True
    save_events(client, events, ntfy=True)
    legacy = {key: {name: value for name, value in config.items() if name != "email"} for key, config in events.items()}
    legacy["ivr_session_failed"]["priority"] = "max"
    enable_ntfy(client, events=legacy, token=None)
    after = current_events(client)
    assert after["ivr_session_failed"]["email"] is True
    assert after["ivr_session_failed"]["priority"] == "max"
    after["ivr_session_failed"]["email"] = False
    save_events(client, after, ntfy=True)
    assert current_events(client)["ivr_session_failed"]["email"] is False


def test_email_test_requires_enabled_settings(client) -> None:
    assert client.post(f"{EMAIL_URL}/test").status_code == 400
    enable_email(client, enabled=False)
    response = client.post(f"{EMAIL_URL}/test")
    assert response.status_code == 400
    assert "Enable email" in response.json()["detail"]


def test_email_test_login_failure_is_sanitized(client, monkeypatch, caplog) -> None:
    reply = f"5.7.8 Authentication failed for {USERNAME} using {PASSWORD}\r\nTry again".encode()
    calls = fake_smtp(monkeypatch, login_error=smtplib.SMTPAuthenticationError(535, reply))
    enable_email(client)
    response = client.post(f"{EMAIL_URL}/test")
    assert response.status_code == 502
    detail = response.json()["detail"]
    assert detail.startswith("SMTP login failed (535 5.7.8 Authentication failed")
    assert PASSWORD not in detail
    assert USERNAME not in detail
    assert "\n" not in detail
    assert calls[-1] == ("quit",)
    audit = json.dumps(client.get("/api/admin/v1/audit").json())
    assert PASSWORD not in audit
    assert USERNAME not in audit
    assert "email.test_sent" not in audit
    assert PASSWORD not in caplog.text


def test_email_test_never_logs_in_without_starttls(client, monkeypatch) -> None:
    calls = fake_smtp(
        monkeypatch,
        starttls_error=smtplib.SMTPNotSupportedError("STARTTLS extension not supported by server."),
    )
    enable_email(client)
    response = client.post(f"{EMAIL_URL}/test")
    assert response.status_code == 502
    assert "STARTTLS" in response.json()["detail"]
    assert [call[0] for call in calls] == ["connect", "ehlo", "starttls", "quit"]


def test_email_test_sends_over_verified_starttls(client, monkeypatch) -> None:
    calls = fake_smtp(monkeypatch)
    enable_email(client, recipients=["alerts@example.com", "second@example.com"])
    response = client.post(f"{EMAIL_URL}/test")
    assert response.status_code == 200, response.text
    assert response.json()["password_configured"] is True
    assert [call[0] for call in calls] == ["connect", "ehlo", "starttls", "ehlo", "login", "send", "quit"]
    assert calls[0] == ("connect", "smtp.mail.me.com", 587, {"local_hostname": "ivrdroid.test", "timeout": 10})
    context = calls[2][1]
    assert context.verify_mode == ssl.CERT_REQUIRED
    assert context.check_hostname is True
    assert calls[4] == ("login", USERNAME, PASSWORD)
    _, message, from_addr, to_addrs = calls[5]
    assert from_addr == "ivrdroid@icloud.com"
    assert to_addrs == ["alerts@example.com", "second@example.com"]
    assert message["From"] == "IVRdroid <ivrdroid@icloud.com>"
    assert message["To"] == "alerts@example.com, second@example.com"
    assert message["Subject"] == "[IVRdroid] IVRdroid test"
    assert message["Date"]
    assert message["Message-ID"].endswith("@icloud.com>")
    assert message["Auto-Submitted"] == "auto-generated"
    assert "https://ivrdroid.test/settings" in message.get_content()
    audit = client.get("/api/admin/v1/audit").json()
    assert any(item["action"] == "email.test_sent" for item in audit)


def test_email_test_ssl_mode_uses_implicit_tls(client, monkeypatch) -> None:
    calls = fake_smtp(monkeypatch, "SMTP_SSL")
    enable_email(client, smtp_security="ssl", smtp_port=465)
    response = client.post(f"{EMAIL_URL}/test")
    assert response.status_code == 200, response.text
    _, host, port, options = calls[0]
    assert (host, port) == ("smtp.mail.me.com", 465)
    assert options["context"].verify_mode == ssl.CERT_REQUIRED
    assert [call[0] for call in calls] == ["connect", "login", "send", "quit"]


def test_dispatched_email_failure_is_logged_without_secrets(client, monkeypatch, caplog) -> None:
    fake_smtp(monkeypatch, login_error=smtplib.SMTPAuthenticationError(535, f"rejected {PASSWORD}".encode()))
    enable_email(client)
    events = current_events(client)
    events["ivr_session_failed"]["email"] = True
    save_events(client, events, ntfy=False)
    # Alembic's fileConfig in the migration tests disables loggers that already exist.
    monkeypatch.setattr(logging.getLogger("ivrdroid.email"), "disabled", False)
    with caplog.at_level(logging.WARNING, logger="ivrdroid.email"):
        dispatch_ntfy(client.app, "ivr_session_failed", title="IVR session failed", message="HELPER_BUSY")
    assert "SMTP login failed (535 rejected ***)" in caplog.text
    assert PASSWORD not in caplog.text


@pytest.mark.parametrize(
    ("error", "expected"),
    [
        (socket.gaierror(8, "nodename nor servname provided"), "SMTP host smtp.mail.me.com was not found"),
        (TimeoutError("timed out"), "smtp.mail.me.com:587 timed out; the port may be blocked"),
        (ConnectionRefusedError(61, "Connection refused"), "smtp.mail.me.com:587 was refused"),
        (ssl.SSLError(1, "wrong version number"), "465 uses SSL, 587 uses STARTTLS"),
        (ssl.SSLCertVerificationError(1, "certificate verify failed"), "could not be verified"),
        (
            smtplib.SMTPRecipientsRefused({"alerts@example.com": (550, b"5.1.1 unknown user")}),
            "refused all 1 recipients (550 5.1.1 unknown user)",
        ),
        (smtplib.SMTPSenderRefused(553, b"5.7.1 not owned", "ivrdroid@icloud.com"), "refused the sender address (553"),
    ],
)
def test_smtp_errors_are_described_for_the_owner(error, expected) -> None:
    payload = EmailPayload(
        host="smtp.mail.me.com",
        port=587,
        security="starttls",
        username=USERNAME,
        password=PASSWORD,
        from_address="ivrdroid@icloud.com",
        from_name="IVRdroid",
        recipients=("alerts@example.com",),
        subject="[IVRdroid] test",
        body="test",
        local_hostname="ivrdroid.test",
    )
    description = describe_smtp_error(error, payload)
    assert expected in description
    assert len(description) <= 200
