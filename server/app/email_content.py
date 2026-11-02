"""Wording of notification emails.

Emails carry no links. A short body around one link with an opaque ID is the shape of voicemail
phishing, and filters sent these to spam. Instead each email states the facts and names the
dashboard page to open. Push notifications keep their click target. Subjects stay ASCII and lines
stay short so the message goes out as plain 7-bit text rather than an encoded one.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date, datetime, timezone
from zoneinfo import ZoneInfo

from .models import Recording

# Same titles as the dashboard's notification settings, so the footer names a switch the owner can find.
EVENT_LABELS = {
    "voicemail_ready": "Voicemail ready",
    "conversation_ready": "Conversation ready",
    "ivr_session_failed": "IVR session failed",
    "external_call_failed": "External call failed",
    "revision_activation_failed": "Revision activation failed",
    "storage_full": "Storage full",
    "tablet_offline": "Tablet offline",
    "ivr_session_completed": "IVR session completed",
    "stock_dialer_routing": "Stock-dialer routing",
}
# Same wording as the dashboard's Recordings page.
STOP_REASONS = {
    "session_complete": "Session complete",
    "preempted": "Preempted",
    "capture_failure": "Capture failure",
    "storage_full": "Storage full",
    "interrupted": "Interrupted",
    "writer_failure": "Recording failure",
    "finish_key": "Finish key",
    "maximum_duration": "Time limit",
    "caller_hangup": "Caller hung up",
    "operator_hangup": "Operator disconnected",
    "recording_failure": "Recording failure",
    "buffer_overrun": "Audio buffer overrun",
    "max_call_duration": "Maximum call duration",
    "completed": "Conversation complete",
}
PERSIAN_MONTHS = (
    "Farvardin", "Ordibehesht", "Khordad", "Tir", "Mordad", "Shahrivar",
    "Mehr", "Aban", "Azar", "Dey", "Bahman", "Esfand",
)
GREGORIAN_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
LISTEN_HINT = "To listen, open the IVRdroid dashboard and choose Recordings."


@dataclass(frozen=True)
class RecordingFacts:
    kind: str
    captured_at: datetime
    duration_ms: int
    operator_last4: str
    stop_reason: str
    partial: bool
    size_bytes: int | None

    @classmethod
    def of(cls, recording: Recording) -> RecordingFacts:
        return cls(
            kind=recording.kind,
            captured_at=recording.captured_at,
            duration_ms=recording.duration_ms,
            operator_last4=recording.operator_last4,
            stop_reason=recording.stop_reason,
            partial=recording.partial,
            size_bytes=recording.media_size_bytes,
        )


def jalali_date(day: date) -> tuple[int, int, int]:
    # 33-year arithmetic cycle; matches the dashboard's Intl Persian calendar.
    month_offsets = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)
    year = day.year + 1 if day.month > 2 else day.year
    days = (
        355666 + 365 * day.year + (year + 3) // 4 - (year + 99) // 100 + (year + 399) // 400
        + day.day + month_offsets[day.month - 1]
    )
    jalali_year = -1595 + 33 * (days // 12053)
    days %= 12053
    jalali_year += 4 * (days // 1461)
    days %= 1461
    if days > 365:
        jalali_year += (days - 1) // 365
        days = (days - 1) % 365
    if days < 186:
        return jalali_year, 1 + days // 31, 1 + days % 31
    return jalali_year, 7 + (days - 186) // 30, 1 + (days - 186) % 30


def format_when(moment: datetime, timezone_name: str, calendar: str) -> str:
    if moment.tzinfo is None:
        # SQLite drops the offset; stored timestamps are always UTC.
        moment = moment.replace(tzinfo=timezone.utc)
    local = moment.astimezone(ZoneInfo(timezone_name))
    if calendar == "persian":
        year, month, day = jalali_date(local.date())
        month_name = PERSIAN_MONTHS[month - 1]
    else:
        year, month_name, day = local.year, GREGORIAN_MONTHS[local.month - 1], local.day
    return f"{month_name} {day}, {year}, {local:%H:%M} ({timezone_name})"


def format_duration(duration_ms: int) -> str:
    seconds = max(0, round(duration_ms / 1000))
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    if hours:
        return f"{hours} h {minutes} min"
    if minutes:
        return f"{minutes} min {seconds} s"
    return f"{seconds} s"


def format_size(size_bytes: int) -> str:
    if size_bytes < 1_000_000:
        return f"{max(1, round(size_bytes / 1000))} KB"
    return f"{size_bytes / 1_000_000:.1f} MB"


def caller_ending(caller_masked: str | None) -> str | None:
    """Last four digits from a masked number; masks like +98-***-5356 read as phishing in email."""
    if not caller_masked:
        return None
    digits = caller_masked[-4:]
    return digits if digits.isdigit() else None


def caller_line(caller_masked: str) -> str:
    digits = caller_ending(caller_masked)
    return f"Caller: number ending {digits}" if digits else "Caller: short or hidden number"


def recording_email(
    facts: RecordingFacts,
    *,
    caller_masked: str | None,
    timezone_name: str,
    calendar: str,
) -> tuple[str, str]:
    conversation = facts.kind == "conversation"
    digits = caller_ending(caller_masked)
    duration = format_duration(facts.duration_ms)
    noun = "Call" if conversation else "Voicemail"
    subject = f"{noun} from number ending {digits}, {duration}" if digits else (
        f"Recorded call, {duration}" if conversation else f"Voicemail, {duration}"
    )
    lines = [f"When: {format_when(facts.captured_at, timezone_name, calendar)}"]
    if caller_masked:
        lines.append(caller_line(caller_masked))
    lines.append(f"Duration: {duration}")
    if conversation and facts.operator_last4:
        lines.append(f"Answered by: operator line ending {facts.operator_last4}")
    lines.append(f"Ended: {STOP_REASONS.get(facts.stop_reason, facts.stop_reason.replace('_', ' ').capitalize())}")
    status = "Partial audio" if facts.partial else "Ready"
    lines.append(f"Recording: {status}, {format_size(facts.size_bytes)}" if facts.size_bytes else f"Recording: {status}")
    intro = (
        "A forwarded call was recorded and saved in IVRdroid."
        if conversation
        else "A caller left a voicemail in IVRdroid."
    )
    return subject, "\n\n".join([intro, "\n".join(lines), LISTEN_HINT])


def event_email(message: str, *, caller_masked: str | None) -> str:
    text = f"{message}\n{caller_line(caller_masked)}" if caller_masked else message
    return f"{text}\n\nOpen the IVRdroid dashboard for details."


def with_footer(body: str, note: str) -> str:
    return f"{body}\n\n{note}\n"


def event_note(event_key: str) -> str:
    return f'Sent because "{EVENT_LABELS.get(event_key, event_key)}" email is on in IVRdroid settings.'
