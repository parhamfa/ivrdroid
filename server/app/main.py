from __future__ import annotations

from .audit_settings import acknowledge_policy, router as audit_settings_router, signed_policy, validate_call_report
from .call_safety_settings import acknowledge_call_safety_policy, router as call_safety_settings_router, signed_call_safety_policy
from .continuous_recording_api import router as continuous_recording_router
from .recording_recovery import router as recording_recovery_router
from .continuous_recording_worker import process_next as process_next_continuous_recording
from .session_audit_api import router as session_audit_router
from .release import VERSION, SOURCE_COMMIT

import asyncio
import base64
import json
import logging
import os
import secrets
import tempfile
from contextlib import asynccontextmanager
from contextlib import suppress
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Annotated

from fastapi import (
    APIRouter,
    BackgroundTasks,
    Depends,
    FastAPI,
    File,
    Form,
    Header,
    HTTPException,
    Request,
    UploadFile,
    status,
)
from fastapi.responses import FileResponse, RedirectResponse
from sqlalchemy import Integer, delete, func, select, update
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write, require_device
from .config import Settings, load_settings
from .crypto import DataCipher, RevisionSigner, digest_device_token, digest_pairing_code, mask_phone
from .database import Base, Database, get_session
from .models import (
    AuditLog,
    CallRecord,
    Device,
    Draft,
    EnrollmentAttempt,
    PairingCode,
    Prompt,
    Recording,
    Revision,
    utcnow,
)
from .diff_v2 import diff_flows
from .flow_v2 import block_label, walk_blocks
from .prompt_service import PromptImportError, import_prompt
from .schemas import (
    AuditResponse,
    CallResponse,
    CallerPolicy,
    DeviceEnrollmentRequest,
    DeviceEnrollmentResponse,
    DeviceResponse,
    DeviceSyncRequest,
    DeviceSyncResponse,
    DraftConfiguration,
    DraftConfigurationV2,
    DraftConfigurationV3,
    DraftConfigurationV4,
    EventBatchRequest,
    PairingCodeRequest,
    PairingCodeResponse,
    PromptResponse,
    RevisionAckRequest,
    RevisionResponse,
    RevisionDetail,
    FlowDefinitionV2,
    FlowDiffRequest,
    FlowDiffResult,
    SimulationRequest,
    SimulationResult,
    ValidationResult,
)
from .services import (
    DraftConflictError,
    V4_DRAFT_ID,
    audit,
    create_revision,
    create_revision_v2,
    initialize_v3_draft,
    initialize_v4_draft,
    load_draft,
    load_draft_v2,
    load_draft_v3,
    load_draft_v4,
    load_manifest,
    prompts_for_configuration,
    prompts_for_configuration_v2,
    store_draft,
    store_draft_v2,
)
from .simulation_v2 import simulate
from .validation import validate_configuration, validate_configuration_v2
from .recording_api import (
    admin_router as recording_admin_router,
    device_router as recording_device_router,
    settings_router as recording_settings_router,
)
from .recording_service import (
    RecordingCipher,
    cleanup_abandoned_uploads,
    discard_quarantined_media,
    ensure_recording_directories,
    initialize_retention_policy,
    purge_expired_recordings,
    reconcile_quarantined_media,
    reconcile_recording_storage,
    remove_upload_files,
    restore_quarantined_media,
)
from .ntfy import (
    call_notification_kwargs,
    classify_call_notifications,
    run_ntfy_watch,
    schedule_ntfy,
)
from .ntfy_api import router as ntfy_settings_router
from .v3_api import router as admin_v3_router
from .v4_api import router as admin_v4_router
from .conversation_recording_api import router as conversation_recording_device_router


SOURCE_URL = "https://github.com/parhamfa/ivrdroid"
LOGGER = logging.getLogger("ivrdroid.recording-maintenance")


def _development_keys(settings: Settings) -> None:
    if settings.environment == "production":
        return
    for attribute in (
        "config_signing_private_key_b64",
        "data_encryption_key_b64",
        "pairing_hmac_key_b64",
        "recording_encryption_key_b64",
    ):
        if not getattr(settings, attribute):
            setattr(settings, attribute, base64.b64encode(os.urandom(32)).decode("ascii"))


def create_app(settings: Settings | None = None) -> FastAPI:
    configured = settings or load_settings()
    configured.validate_runtime()
    _development_keys(configured)
    database = Database(configured.database_url)
    cipher = DataCipher(configured.data_encryption_key_b64)
    signer = RevisionSigner(configured.config_signing_private_key_b64)
    recording_cipher = RecordingCipher(
        configured.recording_encryption_key_b64,
        configured.recording_encryption_key_version,
        configured.recording_encryption_previous_keys_json,
    )
    ensure_recording_directories(configured)

    def maintain_recordings() -> None:
        quarantined = []
        abandoned = []
        with database.session() as session:
            try:
                reconcile_quarantined_media(session, configured)
                reconcile_recording_storage(session, configured)
                policy = initialize_retention_policy(
                    session,
                    configured.development_admin_email,
                )
                quarantined = purge_expired_recordings(session, configured, policy)
                abandoned = cleanup_abandoned_uploads(session, configured)
                session.commit()
            except Exception:
                session.rollback()
                for item in reversed(quarantined):
                    restore_quarantined_media(configured, item)
                raise
        for item in quarantined:
            discard_quarantined_media(configured, item)
        for upload in abandoned:
            remove_upload_files(configured, upload)

    async def recording_maintenance_loop() -> None:
        while True:
            await asyncio.sleep(3600)
            try:
                await asyncio.to_thread(maintain_recordings)
            except Exception:
                LOGGER.exception("Scheduled recording maintenance failed")

    async def ntfy_watch_loop(application: FastAPI) -> None:
        while True:
            await asyncio.sleep(60)
            try:
                await asyncio.to_thread(run_ntfy_watch, application)
            except Exception:
                LOGGER.exception("Scheduled ntfy watch failed")

    async def continuous_recording_loop(application: FastAPI) -> None:
        while True:
            await asyncio.sleep(2)
            try:
                await asyncio.to_thread(process_next_continuous_recording, application)
            except Exception:
                LOGGER.exception("Continuous recording worker failed")

    @asynccontextmanager
    async def lifespan(application: FastAPI):
        if configured.auto_create_schema:
            Base.metadata.create_all(database.engine)
        with database.session() as session:
            initialize_v3_draft(
                session,
                cipher,
                configured.development_admin_email,
            )
            session.flush()
            initialize_v4_draft(
                session,
                cipher,
                configured.development_admin_email,
            )
            session.commit()
        maintain_recordings()
        maintenance = asyncio.create_task(recording_maintenance_loop())
        watch = asyncio.create_task(ntfy_watch_loop(application))
        continuous = asyncio.create_task(continuous_recording_loop(application))
        try:
            yield
        finally:
            maintenance.cancel()
            watch.cancel()
            continuous.cancel()
            with suppress(asyncio.CancelledError):
                await maintenance
            with suppress(asyncio.CancelledError):
                await watch
            with suppress(asyncio.CancelledError):
                await continuous

    app = FastAPI(
        title="IVRdroid API",
        version=VERSION,
        docs_url=None if configured.environment == "production" else "/docs",
        redoc_url=None,
        lifespan=lifespan,
    )
    app.state.settings = configured
    app.state.database = database
    app.state.cipher = cipher
    app.state.signer = signer
    app.state.recording_cipher = recording_cipher
    app.include_router(admin_router)
    app.include_router(admin_v2_router)
    app.include_router(admin_v3_router)
    app.include_router(admin_v4_router)
    app.include_router(device_router)
    app.include_router(recording_admin_router)
    app.include_router(recording_settings_router)
    app.include_router(ntfy_settings_router)
    app.include_router(recording_device_router)
    app.include_router(conversation_recording_device_router)
    app.include_router(audit_settings_router)
    app.include_router(call_safety_settings_router)
    app.include_router(continuous_recording_router)
    app.include_router(recording_recovery_router)
    app.include_router(session_audit_router)

    @app.get("/health")
    def health(session: Session = Depends(get_session)) -> dict:
        session.execute(select(1))
        return {"status": "ok", "service": "ivrdroid-api", "version": VERSION, "source_commit": SOURCE_COMMIT}

    @app.get("/source", include_in_schema=False)
    def source() -> RedirectResponse:
        return RedirectResponse(SOURCE_URL, status_code=302)

    return app


admin_router = APIRouter(prefix="/api/admin/v1", tags=["admin"])
admin_v2_router = APIRouter(prefix="/api/admin/v2", tags=["admin-v2"])
device_router = APIRouter(prefix="/api/device/v1", tags=["device"])


def _prompt_usage(
    configuration: DraftConfiguration | DraftConfigurationV2 | DraftConfigurationV3 | DraftConfigurationV4,
) -> dict[str, list[str]]:
    usage: dict[str, list[str]] = {}
    if isinstance(configuration, (DraftConfigurationV2, DraftConfigurationV3, DraftConfigurationV4)):
        for block, _ in walk_blocks(configuration.flow.root):
            prompt_id = getattr(block, "prompt_id", None)
            if prompt_id:
                usage.setdefault(prompt_id, []).append(f"IVR Flow · {block_label(block)}")
        return usage
    for node in configuration.flow.nodes:
        prompt_id = getattr(node, "prompt_id", None)
        if prompt_id:
            usage.setdefault(prompt_id, []).append(f"IVR Flow · {node.id}")
    return usage


def _prompt_response(prompt: Prompt, usage: list[str]) -> PromptResponse:
    return PromptResponse(
        id=prompt.id,
        name=prompt.name,
        version=prompt.version,
        content_hash=prompt.content_hash,
        size_bytes=prompt.size_bytes,
        duration_ms=prompt.duration_ms,
        created_at=prompt.created_at,
        used_by=usage,
        audio_url=f"/api/admin/v1/prompts/{prompt.id}/audio",
    )


def _revision_response(revision: Revision) -> RevisionResponse:
    return RevisionResponse(
        id=revision.id,
        schema_version=revision.schema_version,
        manifest_sha256=revision.manifest_sha256,
        signature_b64=revision.signature_b64,
        source_revision_id=revision.source_revision_id,
        published_at=revision.published_at,
        published_by=revision.published_by,
    )


def _current_configuration(
    session: Session,
    cipher: DataCipher,
) -> DraftConfiguration | DraftConfigurationV2 | DraftConfigurationV3 | DraftConfigurationV4:
    draft = session.get(Draft, 1)
    if draft is None:
        return DraftConfigurationV3()
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") == 2:
        return load_draft_v2(session, cipher)
    if document.get("schema_version") == 3:
        return load_draft_v3(session, cipher)
    if document.get("schema_version") == 4:
        return load_draft_v4(session, cipher)
    return DraftConfiguration.model_validate(document)


def _overview_caller_policy(
    session: Session,
    cipher: DataCipher,
    device: Device | None,
) -> CallerPolicy:
    """Return the policy the tablet is actually running when one is active."""
    if device is not None and device.active_revision_id is not None:
        active_revision = session.get(Revision, device.active_revision_id)
        if active_revision is not None:
            manifest = load_manifest(active_revision, cipher)
            return CallerPolicy.model_validate(manifest["caller_policy"])

    if session.get(Draft, V4_DRAFT_ID) is not None:
        return load_draft_v4(session, cipher).caller_policy
    return _current_configuration(session, cipher).caller_policy


def _editable_configurations(
    session: Session,
    cipher: DataCipher,
) -> list[DraftConfiguration | DraftConfigurationV2 | DraftConfigurationV3 | DraftConfigurationV4]:
    configurations = [_current_configuration(session, cipher)]
    if session.get(Draft, V4_DRAFT_ID) is not None:
        configurations.append(load_draft_v4(session, cipher))
    return configurations


def _combined_prompt_usage(
    session: Session,
    cipher: DataCipher,
) -> dict[str, list[str]]:
    combined: dict[str, list[str]] = {}
    for configuration in _editable_configurations(session, cipher):
        for prompt_id, labels in _prompt_usage(configuration).items():
            combined.setdefault(prompt_id, []).extend(labels)
    return combined


def _validation_errors(
    request: Request,
    session: Session,
    configuration: DraftConfiguration,
) -> list[str]:
    prompts = prompts_for_configuration(session, configuration)
    return validate_configuration(
        configuration,
        {prompt.id for prompt in prompts},
        {prompt.id: prompt.size_bytes for prompt in prompts},
        request.app.state.settings.revision_asset_limit_bytes,
    )


def _validation_errors_v2(
    request: Request,
    session: Session,
    configuration: DraftConfigurationV2,
) -> list[str]:
    prompts = prompts_for_configuration_v2(session, configuration)
    return validate_configuration_v2(
        configuration,
        {prompt.id for prompt in prompts},
        {prompt.id: prompt.size_bytes for prompt in prompts},
        request.app.state.settings.revision_asset_limit_bytes,
    )


@admin_router.get("/me")
def admin_me(request: Request, actor: str = Depends(require_admin)) -> dict:
    return {
        "email": actor,
        "source_url": SOURCE_URL,
        "signing_public_key_b64": request.app.state.signer.public_key_b64(),
    }


@admin_router.get("/overview")
def overview(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> dict:
    device = session.scalar(select(Device).order_by(Device.enrolled_at.asc()))
    call_count = session.scalar(select(func.count()).select_from(CallRecord)) or 0
    recording_count = session.scalar(
        select(func.count()).select_from(Recording).where(Recording.status == "ready"),
    ) or 0
    pending_recording_count = session.scalar(
        select(func.count()).select_from(Recording).where(
            Recording.status.in_(["uploading", "processing"]),
        ),
    ) or 0
    prompt_bytes = session.scalar(select(func.coalesce(func.sum(Prompt.size_bytes), 0))) or 0
    caller_policy = _overview_caller_policy(session, request.app.state.cipher, device)
    return {
        "device": None if device is None else _device_document(device),
        "active_revision": None if device is None else device.active_revision_id,
        "call_count": call_count,
        "recording_count": recording_count,
        "pending_recording_count": pending_recording_count,
        "prompt_bytes": prompt_bytes,
        "prompt_quota_bytes": request.app.state.settings.prompt_quota_bytes,
        "caller_policy": caller_policy.model_dump(mode="json"),
    }


@admin_v2_router.get("/draft", response_model=DraftConfigurationV2)
def get_draft_v2(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> DraftConfigurationV2:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow authoring V2 is archived and read-only. Use /api/admin/v4/draft.",
    )


@admin_v2_router.put("/draft", response_model=DraftConfigurationV2)
def put_draft_v2(
    configuration: DraftConfigurationV2,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> DraftConfigurationV2:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow authoring V2 is archived and read-only. Use /api/admin/v4/draft.",
    )


@admin_v2_router.post("/draft/validate", response_model=ValidationResult)
def validate_draft_v2(
    request: Request,
    configuration: DraftConfigurationV2 | None = None,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> ValidationResult:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow validation V2 is archived. Use /api/admin/v4/draft/validate.",
    )


@admin_v2_router.post("/draft/simulate", response_model=SimulationResult)
def simulate_draft_v2(
    body: SimulationRequest,
    request: Request,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> dict:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow simulation V2 is archived. Use /api/admin/v4/draft/simulate.",
    )


@admin_v2_router.post("/draft/diff", response_model=FlowDiffResult)
def diff_draft_v2(
    body: FlowDiffRequest,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> FlowDiffResult:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow diff V2 is archived. Use /api/admin/v4/draft/diff.",
    )


@admin_v2_router.post("/draft/publish", response_model=RevisionResponse)
def publish_draft_v2(
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow publishing V2 is archived. Use /api/admin/v4/draft/publish.",
    )


@admin_v2_router.get("/revisions/{revision_id}", response_model=RevisionDetail)
def revision_detail_v2(
    revision_id: int,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> RevisionDetail:
    revision = session.get(Revision, revision_id)
    if revision is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    manifest = load_manifest(revision, request.app.state.cipher)
    return RevisionDetail(
        revision=_revision_response(revision),
        schema_version=revision.schema_version,
        flow=manifest.get("flow"),
        legacy=revision.schema_version != 2,
    )


@admin_router.get("/draft", response_model=DraftConfiguration)
def get_draft(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> DraftConfiguration:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow authoring V1 is archived and read-only. Use /api/admin/v4/draft.",
    )


@admin_router.put("/draft", response_model=DraftConfiguration)
def put_draft(
    configuration: DraftConfiguration,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> DraftConfiguration:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow authoring V1 is archived and read-only. Use /api/admin/v4/draft.",
    )


@admin_router.post("/draft/validate", response_model=ValidationResult)
def validate_draft(
    request: Request,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> ValidationResult:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow authoring V1 is archived and read-only. Use /api/admin/v4/draft/validate.",
    )


@admin_router.post("/draft/publish", response_model=RevisionResponse)
def publish_draft(
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    raise HTTPException(
        status_code=status.HTTP_410_GONE,
        detail="Flow publishing V1 is archived and read-only. Use /api/admin/v4/draft/publish.",
    )


@admin_router.get("/revisions", response_model=list[RevisionResponse])
def list_revisions(
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> list[RevisionResponse]:
    revisions = session.scalars(select(Revision).order_by(Revision.id.desc())).all()
    return [_revision_response(item) for item in revisions]


@admin_v2_router.post("/revisions/{revision_id}/rollback", response_model=RevisionResponse)
def rollback_revision(
    revision_id: int,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version != 2:
        raise HTTPException(status_code=409, detail="This endpoint accepts immutable V2 revisions only.")
    affected = session.execute(
        update(Device)
        .where(Device.revoked_at.is_(None))
        .values(desired_revision_id=source.id),
    ).rowcount
    audit(
        session,
        actor,
        "revision.legacy_rollback_activated",
        f"revision:{source.id}",
        {"schema_version": 2, "affected_devices": affected},
    )
    session.commit()
    return _revision_response(source)


@admin_v2_router.post(
    "/revisions/{revision_id}/activate-legacy",
    response_model=RevisionResponse,
)
def activate_legacy_revision(
    revision_id: int,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    """Point enrolled devices at an existing signed V1 snapshot for cutover rollback."""
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version != 1:
        raise HTTPException(
            status_code=409,
            detail="Emergency legacy activation accepts signed V1 revisions only.",
        )
    affected = session.execute(
        update(Device)
        .where(Device.revoked_at.is_(None))
        .values(desired_revision_id=source.id),
    ).rowcount
    audit(
        session,
        actor,
        "revision.legacy_rollback_activated",
        f"revision:{source.id}",
        {"affected_devices": affected},
    )
    session.commit()
    return _revision_response(source)


@admin_router.get("/prompts", response_model=list[PromptResponse])
def list_prompts(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> list[PromptResponse]:
    usage = _combined_prompt_usage(session, request.app.state.cipher)
    prompts = session.scalars(select(Prompt).order_by(Prompt.created_at.desc())).all()
    return [_prompt_response(prompt, usage.get(prompt.id, [])) for prompt in prompts]


@admin_router.post("/prompts", response_model=PromptResponse, status_code=201)
def upload_prompt(
    request: Request,
    name: Annotated[str, Form(min_length=1, max_length=120)],
    file: Annotated[UploadFile, File()],
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> PromptResponse:
    settings = request.app.state.settings
    suffix = Path(file.filename or "").suffix.lower()
    source_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            suffix=suffix,
            prefix="upload-",
            dir=settings.media_root,
            delete=False,
        ) as temporary:
            source_path = Path(temporary.name)
            total = 0
            while chunk := file.file.read(1024 * 1024):
                total += len(chunk)
                if total > settings.prompt_upload_limit_bytes:
                    raise HTTPException(status_code=413, detail="Prompt upload exceeds 25 MiB")
                temporary.write(chunk)
        prompt = import_prompt(
            session,
            settings,
            name=name,
            source_path=source_path,
            actor=actor,
        )
        session.commit()
        return _prompt_response(prompt, [])
    except PromptImportError as error:
        raise HTTPException(status_code=error.status_code, detail=str(error)) from error
    finally:
        if source_path is not None:
            source_path.unlink(missing_ok=True)


@admin_router.get("/prompts/{prompt_id}/audio")
def admin_prompt_audio(
    prompt_id: str,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> FileResponse:
    prompt = session.get(Prompt, prompt_id)
    if prompt is None:
        raise HTTPException(status_code=404, detail="Prompt not found")
    path = request.app.state.settings.media_root / prompt.storage_name
    if not path.is_file():
        raise HTTPException(status_code=503, detail="Prompt media is unavailable")
    return FileResponse(path, media_type="audio/wav", filename=f"{prompt.name}.wav")


@admin_router.delete("/prompts/{prompt_id}", status_code=204)
def delete_prompt(
    prompt_id: str,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> None:
    prompt = session.get(Prompt, prompt_id)
    if prompt is None:
        raise HTTPException(status_code=404, detail="Prompt not found")
    if prompt_id in _combined_prompt_usage(session, request.app.state.cipher):
        raise HTTPException(status_code=409, detail="Prompt is used by the current draft")
    for revision in session.scalars(select(Revision)):
        manifest = load_manifest(revision, request.app.state.cipher)
        if any(item["id"] == prompt_id for item in manifest.get("prompts", [])):
            raise HTTPException(status_code=409, detail="Published revisions retain this prompt")
    path = request.app.state.settings.media_root / prompt.storage_name
    session.delete(prompt)
    audit(session, actor, "prompt.deleted", f"prompt:{prompt_id}")
    session.commit()
    path.unlink(missing_ok=True)


@admin_router.post("/pairing-codes", response_model=PairingCodeResponse, status_code=201)
def create_pairing_code(
    body: PairingCodeRequest,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> PairingCodeResponse:
    settings = request.app.state.settings
    for _ in range(10):
        code = f"{secrets.randbelow(100_000_000):08d}"
        digest = digest_pairing_code(code, settings.pairing_hmac_key_b64)
        if session.scalar(select(PairingCode).where(PairingCode.code_digest == digest)) is None:
            break
    else:  # pragma: no cover - cryptographically implausible
        raise HTTPException(status_code=503, detail="Could not allocate pairing code")
    expires_at = utcnow() + timedelta(seconds=settings.enrollment_code_ttl_seconds)
    pairing = PairingCode(
        code_digest=digest,
        display_name=body.display_name,
        expires_at=expires_at,
        created_by=actor,
    )
    session.add(pairing)
    session.flush()
    audit(session, actor, "device.pairing_code_created", f"pairing:{pairing.id}")
    session.commit()
    return PairingCodeResponse(code=code, expires_at=expires_at)


def _device_document(device: Device) -> dict:
    return {
        "id": device.id,
        "display_name": device.display_name,
        "app_version": device.app_version,
        "helper_version": device.helper_version,
        "desired_revision_id": device.desired_revision_id,
        "active_revision_id": device.active_revision_id,
        "status": device.status or {},
        "last_seen_at": device.last_seen_at,
        "enrolled_at": device.enrolled_at,
        "revoked_at": device.revoked_at,
    }


@admin_router.get("/devices", response_model=list[DeviceResponse])
def list_devices(
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> list[dict]:
    return [
        _device_document(device)
        for device in session.scalars(select(Device).order_by(Device.enrolled_at.asc()))
    ]


@admin_router.post("/devices/{device_id}/revoke", response_model=DeviceResponse)
def revoke_device(
    device_id: str,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> dict:
    device = session.get(Device, device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="Device not found")
    device.revoked_at = utcnow()
    audit(session, actor, "device.revoked", f"device:{device.id}")
    session.commit()
    return _device_document(device)


def call_audit_summary(request: Request, call: CallRecord, recording: Recording | None, *, include_events: bool = False) -> dict | None:
    from .recording_api import _recording_response
    report = dict((recording.audit_metadata if recording else call.session_audit) or {})
    if not report and recording is None:
        return None
    if not include_events:
        report.pop("events", None)
    if recording is not None:
        metadata = _recording_response(request, recording, call).model_dump(mode="json")
        metadata.pop("audit_metadata", None)
        report.update(state=recording.status, recording=metadata)
    return report


@admin_router.get("/calls/{call_id}")
def call_detail(call_id: str, request: Request, _: str = Depends(require_admin), session: Session = Depends(get_session)) -> dict:
    from .recording_api import _recording_response
    call = session.get(CallRecord, call_id)
    if call is None:
        raise HTTPException(status_code=404, detail="Call not found")
    caller = request.app.state.cipher.decrypt(call.caller_encrypted) if call.caller_encrypted else None
    recordings = session.scalars(select(Recording).where(Recording.call_id == call.id, Recording.kind != "session_audit")).all()
    return {
        "id": call.id, "device_id": call.device_id, "started_at": call.started_at,
        "caller": caller, "caller_masked": mask_phone(caller), "policy_decision": call.policy_decision,
        "revision_id": call.revision_id, "menu_path": call.menu_path, "result": call.result,
        "duration_seconds": call.duration_seconds, "events": call.events,
        "session_audit": call_audit_summary(request, call, session.scalar(select(Recording).where(Recording.call_id == call.id, Recording.kind == "session_audit")), include_events=True),
        "recording_count": sum(r.status == "ready" for r in recordings),
        "pending_recording_count": sum(r.status in {"uploading", "processing"} for r in recordings),
        "recordings": [_recording_response(request, r, call).model_dump(mode="json") for r in recordings],
    }


@admin_router.get("/calls", response_model=list[CallResponse])
def list_calls(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> list[CallResponse]:
    calls = session.scalars(
        select(CallRecord).order_by(CallRecord.started_at.desc()).limit(500),
    ).all()
    counts: dict[str, tuple[int, int]] = {}
    call_ids = [call.id for call in calls]
    if call_ids:
        for call_id, total, pending in session.execute(
            select(
                Recording.call_id,
                func.sum((Recording.status == "ready").cast(Integer)),
                func.sum(
                    Recording.status.in_(["uploading", "processing"]).cast(Integer),
                ),
            )
            .where(Recording.call_id.in_(call_ids), Recording.status != "deleted", Recording.kind != "session_audit")
            .group_by(Recording.call_id),
        ):
            counts[call_id] = (int(total or 0), int(pending or 0))
    audit_recordings = {r.call_id: r for r in session.scalars(select(Recording).where(Recording.call_id.in_(call_ids), Recording.kind == "session_audit"))}
    response: list[CallResponse] = []
    for call in calls:
        caller = None
        if call.caller_encrypted:
            caller = request.app.state.cipher.decrypt(call.caller_encrypted)
        response.append(
            CallResponse(
                id=call.id,
                device_id=call.device_id,
                started_at=call.started_at,
                caller=caller,
                caller_masked=mask_phone(caller),
                policy_decision=call.policy_decision,
                revision_id=call.revision_id,
                menu_path=call.menu_path or [],
                result=call.result,
                duration_seconds=call.duration_seconds,
                events=call.events or [],
                session_audit=call_audit_summary(request, call, audit_recordings.get(call.id)),
                recording_count=counts.get(call.id, (0, 0))[0],
                pending_recording_count=counts.get(call.id, (0, 0))[1],
            ),
        )
    return response


@admin_router.get("/audit", response_model=list[AuditResponse])
def list_audit(
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> list[dict]:
    return [
        {
            "actor": item.actor,
            "action": item.action,
            "target": item.target,
            "details": item.details or {},
            "created_at": item.created_at,
        }
        for item in session.scalars(select(AuditLog).order_by(AuditLog.created_at.desc()).limit(500))
    ]


def _source_ip(request: Request, cf_ip: str | None) -> str:
    if request.app.state.settings.environment == "production" and cf_ip:
        return cf_ip[:64]
    return (request.client.host if request.client else "unknown")[:64]


def _record_failed_enrollment(
    session: Session,
    source_ip: str,
    pairing: PairingCode | None,
) -> None:
    if pairing is not None:
        pairing.failures += 1
    session.add(EnrollmentAttempt(source_ip=source_ip, succeeded=False))
    session.commit()


@device_router.post("/enroll", response_model=DeviceEnrollmentResponse)
def enroll_device(
    body: DeviceEnrollmentRequest,
    request: Request,
    cf_ip: str | None = Header(default=None, alias="CF-Connecting-IP"),
    session: Session = Depends(get_session),
) -> DeviceEnrollmentResponse:
    settings = request.app.state.settings
    source_ip = _source_ip(request, cf_ip)
    cutoff = utcnow() - timedelta(seconds=settings.enrollment_failure_window_seconds)
    failures = session.scalar(
        select(func.count())
        .select_from(EnrollmentAttempt)
        .where(
            EnrollmentAttempt.source_ip == source_ip,
            EnrollmentAttempt.succeeded.is_(False),
            EnrollmentAttempt.attempted_at >= cutoff,
        ),
    ) or 0
    if failures >= settings.enrollment_max_failures:
        raise HTTPException(status_code=429, detail="Too many enrollment attempts")

    digest = digest_pairing_code(body.code, settings.pairing_hmac_key_b64)
    pairing = session.scalar(select(PairingCode).where(PairingCode.code_digest == digest))
    pairing_expires_at = None
    if pairing is not None:
        pairing_expires_at = pairing.expires_at
        if pairing_expires_at.tzinfo is None:
            pairing_expires_at = pairing_expires_at.replace(tzinfo=timezone.utc)
    valid = (
        pairing is not None
        and pairing.used_at is None
        and pairing_expires_at is not None
        and pairing_expires_at > utcnow()
        and pairing.failures < settings.enrollment_max_failures
    )
    if not valid:
        _record_failed_enrollment(session, source_ip, pairing)
        raise HTTPException(status_code=401, detail="Pairing code is invalid or expired")

    active_device = session.scalar(select(Device).where(Device.revoked_at.is_(None)))
    if active_device is not None:
        raise HTTPException(
            status_code=409,
            detail="Release one already has an enrolled tablet; revoke it before re-pairing",
        )

    token = secrets.token_urlsafe(32)
    latest_revision = session.scalar(select(Revision).order_by(Revision.id.desc()))
    device = Device(
        display_name=pairing.display_name or body.device_name,
        token_hash=digest_device_token(token),
        app_version=body.app_version,
        helper_version=body.helper_version,
        desired_revision_id=None if latest_revision is None else latest_revision.id,
        status={"enrollment": "complete"},
        last_seen_at=utcnow(),
    )
    pairing.used_at = utcnow()
    session.add(device)
    session.flush()
    session.add(EnrollmentAttempt(source_ip=source_ip, succeeded=True))
    audit(session, f"device:{device.id}", "device.enrolled", f"device:{device.id}")
    session.commit()
    return DeviceEnrollmentResponse(
        device_id=device.id,
        device_token=token,
        service_client_id=settings.cf_device_service_client_id,
        service_client_secret=settings.cf_device_service_client_secret,
        server_url=settings.public_base_url,
    )


@device_router.post("/sync", response_model=DeviceSyncResponse)
def sync_device(
    body: DeviceSyncRequest,
    request: Request,
    background_tasks: BackgroundTasks,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> DeviceSyncResponse:
    acknowledge_policy(session, device, body.status)
    acknowledge_call_safety_policy(session, device, body.status)
    device.app_version = body.app_version
    device.helper_version = body.helper_version
    device.status = body.status.model_dump(mode="json")
    device.last_seen_at = utcnow()
    revision = session.get(Revision, device.desired_revision_id) if device.desired_revision_id else None
    session.commit()
    background_tasks.add_task(run_ntfy_watch, request.app)
    return DeviceSyncResponse(
        audit_policy=signed_policy(request, session),
        call_safety_policy=signed_call_safety_policy(request, session),
        server_time=utcnow(),
        desired_revision_id=device.desired_revision_id,
        active_revision_id=device.active_revision_id,
        manifest_sha256=None if revision is None else revision.manifest_sha256,
        signature_b64=None if revision is None else revision.signature_b64,
    )


@device_router.get("/revisions/{revision_id}/manifest")
def device_manifest(
    revision_id: int,
    request: Request,
    _: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> dict:
    revision = session.get(Revision, revision_id)
    if revision is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    return {
        "manifest": load_manifest(revision, request.app.state.cipher),
        "manifest_sha256": revision.manifest_sha256,
        "signature_b64": revision.signature_b64,
    }


@device_router.get("/prompts/{content_hash}")
def device_prompt(
    content_hash: str,
    request: Request,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> FileResponse:
    if len(content_hash) != 64 or any(character not in "0123456789abcdef" for character in content_hash):
        raise HTTPException(status_code=404, detail="Prompt not found")
    allowed: set[str] = set()
    for revision_id in {device.desired_revision_id, device.active_revision_id} - {None}:
        revision = session.get(Revision, revision_id)
        if revision:
            manifest = load_manifest(revision, request.app.state.cipher)
            allowed.update(item["content_hash"] for item in manifest.get("prompts", []))
    if content_hash not in allowed:
        raise HTTPException(status_code=403, detail="Prompt is not assigned to this device")
    prompt = session.scalar(select(Prompt).where(Prompt.content_hash == content_hash))
    if prompt is None:
        raise HTTPException(status_code=404, detail="Prompt not found")
    path = request.app.state.settings.media_root / prompt.storage_name
    if not path.is_file():
        raise HTTPException(status_code=503, detail="Prompt media is unavailable")
    return FileResponse(path, media_type="audio/wav")


@device_router.post("/revisions/{revision_id}/ack", status_code=204)
def acknowledge_revision(
    revision_id: int,
    body: RevisionAckRequest,
    request: Request,
    background_tasks: BackgroundTasks,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> None:
    if session.get(Revision, revision_id) is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if device.desired_revision_id != revision_id:
        raise HTTPException(status_code=409, detail="Revision is not assigned to this device")
    status_document = dict(device.status or {})
    status_document["revision_state"] = body.state
    if body.error:
        status_document["last_error"] = body.error
    elif body.state == "activated":
        status_document.pop("last_error", None)
    device.status = status_document
    if body.state == "activated":
        device.active_revision_id = revision_id
    device.last_seen_at = utcnow()
    session.commit()
    if body.state == "failed":
        detail = body.error or "The tablet rejected the assigned revision."
        schedule_ntfy(
            background_tasks,
            request.app,
            "revision_activation_failed",
            title="Revision activation failed",
            message=f"Revision {revision_id} failed: {detail}",
            click_path="/settings",
            tags="x,ivrdroid",
        )


@device_router.post("/events:batch")
def upload_events(
    body: EventBatchRequest,
    request: Request,
    background_tasks: BackgroundTasks,
    device: Device = Depends(require_device),
    session: Session = Depends(get_session),
) -> dict:
    accepted: list[str] = []
    pending_notifications: list[tuple[str, dict]] = []
    for call in body.calls:
        validate_call_report(session, call.session_audit, call.result, device.id)
        audit_document = call.session_audit.model_dump(mode="json") if call.session_audit else None
        event_documents = [event.model_dump(mode="json") for event in call.events]
        existing = session.get(CallRecord, call.call_id)
        notify = False
        if existing is not None:
            if existing.device_id != device.id:
                raise HTTPException(status_code=409, detail="Call ID belongs to another device")
            if existing.result == "IN_PROGRESS" and call.result != "IN_PROGRESS":
                existing.menu_path = call.menu_path
                existing.result = call.result
                existing.duration_seconds = call.duration_seconds
                existing.events = event_documents
                existing.session_audit = audit_document
                existing.received_at = utcnow()
                notify = True
            elif audit_document is not None and existing.session_audit is None:
                # A durable audit finalizer may finish after the terminal call event.
                validate_call_report(session, call.session_audit, existing.result, device.id)
                existing.session_audit = audit_document
            elif audit_document is not None and existing.session_audit != audit_document:
                raise HTTPException(status_code=409, detail="A terminal audit report cannot be replaced")
            accepted.append(call.call_id)
        else:
            caller_encrypted = request.app.state.cipher.encrypt(call.caller) if call.caller else None
            session.add(
                CallRecord(
                    id=call.call_id,
                    device_id=device.id,
                    started_at=call.started_at,
                    caller_encrypted=caller_encrypted,
                    caller_last4="" if not call.caller else call.caller[-4:],
                    policy_decision=call.policy_decision,
                    revision_id=call.revision_id,
                    menu_path=call.menu_path,
                    result=call.result,
                    duration_seconds=call.duration_seconds,
                    events=event_documents,
                    session_audit=audit_document,
                ),
            )
            accepted.append(call.call_id)
            notify = call.result != "IN_PROGRESS"
        if notify:
            caller = mask_phone(call.caller) if call.caller else None
            for event_key, extra in classify_call_notifications(
                call.policy_decision,
                call.result,
                event_documents,
            ):
                pending_notifications.append(
                    (
                        event_key,
                        {
                            **call_notification_kwargs(
                                event_key,
                                call_id=call.call_id,
                                result=call.result,
                                policy_decision=call.policy_decision,
                                extra=extra,
                            ),
                            "caller_masked": caller,
                        },
                    ),
                )
    device.last_seen_at = utcnow()
    session.commit()
    for event_key, kwargs in pending_notifications:
        schedule_ntfy(background_tasks, request.app, event_key, **kwargs)
    return {"accepted_call_ids": accepted}


app = create_app()
