from __future__ import annotations

import os
from pathlib import Path
from uuid import uuid4

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from .config import Settings
from .media import MediaError, transcode_prompt
from .models import Prompt
from .services import audit


class PromptImportError(ValueError):
    def __init__(self, message: str, status_code: int = 422):
        super().__init__(message)
        self.status_code = status_code


def import_prompt(
    session: Session,
    settings: Settings,
    *,
    name: str,
    source_path: Path,
    actor: str,
) -> Prompt:
    normalized_name = name.strip()
    if not normalized_name:
        raise PromptImportError("Prompt name cannot be blank")
    if len(normalized_name) > 120:
        raise PromptImportError("Prompt name must be 120 characters or fewer")
    if not source_path.is_file():
        raise PromptImportError("Prompt source file is unavailable")
    if source_path.stat().st_size > settings.prompt_upload_limit_bytes:
        raise PromptImportError("Prompt upload exceeds 25 MiB", status_code=413)

    canonical_temp = settings.media_root / f".{uuid4().hex}.wav"
    destination: Path | None = None
    try:
        canonical = transcode_prompt(
            source_path,
            canonical_temp,
            settings.prompt_duration_limit_seconds,
        )
        existing = session.scalar(select(Prompt).where(Prompt.content_hash == canonical.content_hash))
        if existing:
            raise PromptImportError("This prompt audio already exists", status_code=409)

        used_bytes = session.scalar(select(func.coalesce(func.sum(Prompt.size_bytes), 0))) or 0
        if used_bytes + canonical.size_bytes > settings.prompt_quota_bytes:
            raise PromptImportError("Prompt storage quota exceeded", status_code=507)

        destination = settings.media_root / f"{canonical.content_hash}.wav"
        os.replace(canonical_temp, destination)
        version = (
            session.scalar(select(func.max(Prompt.version)).where(Prompt.name == normalized_name)) or 0
        ) + 1
        prompt = Prompt(
            name=normalized_name,
            version=version,
            content_hash=canonical.content_hash,
            size_bytes=canonical.size_bytes,
            duration_ms=canonical.duration_ms,
            storage_name=destination.name,
            created_by=actor,
        )
        session.add(prompt)
        session.flush()
        audit(
            session,
            actor,
            "prompt.uploaded",
            f"prompt:{prompt.id}",
            {"size_bytes": prompt.size_bytes},
        )
        return prompt
    except MediaError as error:
        raise PromptImportError(str(error)) from error
    except Exception:
        if destination is not None:
            destination.unlink(missing_ok=True)
        raise
    finally:
        canonical_temp.unlink(missing_ok=True)
