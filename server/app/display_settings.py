"""Shared dashboard presentation preferences, independent of signed IVR configuration."""
from typing import Literal
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from fastapi import APIRouter, Depends
from pydantic import Field, field_validator
from sqlalchemy.dialects.postgresql import insert as postgres_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .database import get_session
from .models import DisplaySettings, utcnow
from .schemas import StrictModel
from .services import audit

router = APIRouter(prefix="/api/admin/v1/display-settings", tags=["admin-display"])
DEFAULTS = {"timezone": "Asia/Tehran", "date_calendar": "persian"}


class DisplaySettingsValue(StrictModel):
    timezone: str = Field(min_length=1, max_length=80)
    date_calendar: Literal["gregorian", "persian"]

    @field_validator("timezone")
    @classmethod
    def valid_timezone(cls, value: str) -> str:
        try:
            ZoneInfo(value)
        except (ZoneInfoNotFoundError, ValueError) as error:
            raise ValueError("Choose a valid IANA timezone, such as Asia/Tehran or UTC.") from error
        return value


@router.get("", response_model=DisplaySettingsValue)
def get_settings(
    _: str = Depends(require_admin), session: Session = Depends(get_session),
) -> DisplaySettingsValue:
    row = session.get(DisplaySettings, 1)
    return DisplaySettingsValue(**DEFAULTS) if row is None else DisplaySettingsValue(
        timezone=row.timezone, date_calendar=row.date_calendar,
    )


@router.put("", response_model=DisplaySettingsValue)
def save_settings(
    body: DisplaySettingsValue, actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> DisplaySettingsValue:
    # Atomic upsert also handles a fresh development database and simultaneous first saves.
    insert = sqlite_insert if session.get_bind().dialect.name == "sqlite" else postgres_insert
    values = {**body.model_dump(), "updated_at": utcnow(), "updated_by": actor}
    statement = insert(DisplaySettings).values(id=1, **values)
    session.execute(statement.on_conflict_do_update(index_elements=[DisplaySettings.id], set_=values))
    audit(session, actor, "display.settings_updated", "display-settings", body.model_dump())
    session.commit()
    return body
