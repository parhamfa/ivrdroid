from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .compiler_v4 import compile_flow_v4
from .database import get_session
from .diff_v2 import diff_flows
from .models import Device, Draft, Revision
from .revision_diff import revision_configuration_diff
from .schemas import (
    DraftConfigurationV4,
    FlowDefinitionV2,
    FlowDiffRequestV4,
    FlowDiffResult,
    PublishDraftRequestV4,
    RevisionDetail,
    RevisionResponse,
    SimulationRequestV4,
    SimulationResult,
    ValidationResult,
)
from .services import (
    DraftConflictError,
    V4_DRAFT_ID,
    audit,
    create_revision_v4,
    initialize_v4_draft,
    load_draft_v4,
    load_manifest,
    prompts_for_configuration_v4,
    store_draft_v4,
)
from .simulation_v4 import simulate_v4
from .validation import validate_configuration_v4


router = APIRouter(prefix="/api/admin/v4", tags=["admin-v4"])


def revision_response(revision: Revision) -> RevisionResponse:
    return RevisionResponse(
        id=revision.id,
        schema_version=revision.schema_version,
        manifest_sha256=revision.manifest_sha256,
        signature_b64=revision.signature_b64,
        source_revision_id=revision.source_revision_id,
        published_at=revision.published_at,
        published_by=revision.published_by,
    )


def validation_errors(
    request: Request,
    session: Session,
    configuration: DraftConfigurationV4,
) -> list[str]:
    prompts = prompts_for_configuration_v4(session, configuration)
    errors = validate_configuration_v4(
        configuration,
        {prompt.id for prompt in prompts},
        {prompt.id: prompt.size_bytes for prompt in prompts},
        request.app.state.settings.revision_asset_limit_bytes,
    )
    if not errors:
        try:
            compile_flow_v4(
                configuration.flow,
                configuration.recording_behavior,
                {prompt.id: prompt.duration_ms for prompt in prompts},
            )
        except (TypeError, ValueError) as error:
            errors.append(str(error))
    return list(dict.fromkeys(errors))


def _uses_prompt_barge_in(configuration: DraftConfigurationV4) -> bool:
    pending = [configuration.flow.root] if configuration.flow.root is not None else []
    while pending:
        block = pending.pop()
        if getattr(block, "allow_prompt_barge_in", False):
            return True
        if hasattr(block, "next") and block.next is not None:
            pending.append(block.next)
        for name in (
            "on_timeout",
            "on_invalid",
            "on_return_limit",
            "on_open",
            "on_closed",
            "on_holiday",
            "on_unavailable",
            "on_not_connected",
            "on_system_failure",
        ):
            child = getattr(block, name, None)
            if child is not None:
                pending.append(child)
        pending.extend(
            branch.root
            for branch in getattr(block, "branches", [])
            if branch.root is not None
        )
    return False


def _require_v4_device_capabilities(
    session: Session,
    configuration: DraftConfigurationV4,
) -> None:
    active_devices = list(
        session.scalars(select(Device).where(Device.revoked_at.is_(None))),
    )
    if not active_devices:
        raise HTTPException(
            status_code=409,
            detail="V4 cannot be published until at least one active device reports capability status.",
        )
    prompt_barge_in_required = _uses_prompt_barge_in(configuration)
    incompatible: list[str] = []
    for device in active_devices:
        status = device.status or {}
        versions = status.get("runtime_versions", [1, 2])
        compatible = (
            4 in versions
            and status.get("external_call_control_capable") is True
            and status.get("conversation_recording_capable") is True
            and status.get("call_control_protocol_version") == 1
            and (
                not prompt_barge_in_required
                or status.get("prompt_barge_in_capable") is True
            )
        )
        if not compatible:
            incompatible.append(device.display_name)
    if incompatible:
        prompt_requirement = (
            ", and prompt interruption"
            if prompt_barge_in_required
            else ""
        )
        raise HTTPException(
            status_code=409,
            detail=(
                "V4 cannot be assigned until every active device reports runtime V4, "
                "external-call control, conversation recording, call-control protocol 1"
                f"{prompt_requirement}: "
                + ", ".join(incompatible)
            ),
        )


def _require_immutable_revision_capabilities(
    session: Session,
    revision: Revision,
    manifest: dict,
) -> None:
    recording_required = revision.schema_version == 3 and any(
        instruction.get("op") == "record_message"
        for instruction in manifest.get("program", {}).get("instructions", [])
    )
    incompatible: list[str] = []
    for device in session.scalars(select(Device).where(Device.revoked_at.is_(None))):
        status = device.status or {}
        if revision.schema_version not in status.get("runtime_versions", [1, 2]) or (
            recording_required and status.get("recording_capable") is not True
        ):
            incompatible.append(device.display_name)
    if incompatible:
        raise HTTPException(
            status_code=409,
            detail=f"Rollback revision is incompatible with active devices: {', '.join(incompatible)}",
        )


@router.get("/draft", response_model=DraftConfigurationV4)
def get_draft_v4(
    request: Request,
    actor: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> DraftConfigurationV4:
    draft = session.get(Draft, V4_DRAFT_ID)
    if draft is not None:
        try:
            return load_draft_v4(session, request.app.state.cipher)
        except RuntimeError:
            pass
    cloned = initialize_v4_draft(session, request.app.state.cipher, actor)
    audit(
        session,
        actor,
        "draft.v4_cloned",
        "configuration",
        {"edit_version": cloned.edit_version},
    )
    session.commit()
    return cloned


@router.put("/draft", response_model=DraftConfigurationV4)
def put_draft_v4(
    configuration: DraftConfigurationV4,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> DraftConfigurationV4:
    try:
        saved = store_draft_v4(session, request.app.state.cipher, configuration, actor)
    except DraftConflictError as error:
        raise HTTPException(status_code=409, detail=str(error)) from error
    audit(session, actor, "draft.v4_updated", "configuration", {"edit_version": saved.edit_version})
    session.commit()
    return saved


@router.post("/draft/validate", response_model=ValidationResult)
def validate_draft_v4(
    request: Request,
    configuration: DraftConfigurationV4 | None = None,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> ValidationResult:
    candidate = configuration or load_draft_v4(session, request.app.state.cipher)
    errors = validation_errors(request, session, candidate)
    return ValidationResult(valid=not errors, errors=errors)


@router.post("/draft/simulate", response_model=SimulationResult)
def simulate_draft_v4(
    body: SimulationRequestV4,
    request: Request,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> dict:
    errors = validation_errors(request, session, body.configuration)
    if errors:
        return {"status": "invalid", "trace": [], "message": " ".join(errors)}
    return simulate_v4(body.configuration, body.events)


@router.post("/draft/diff", response_model=FlowDiffResult)
def diff_draft_v4(
    body: FlowDiffRequestV4,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> FlowDiffResult:
    base = session.get(Revision, body.base_revision_id) if body.base_revision_id else session.scalar(
        select(Revision).order_by(Revision.id.desc()),
    )
    if body.base_revision_id is not None and base is None:
        raise HTTPException(status_code=404, detail="Base revision not found")
    legacy_base = base is not None and base.schema_version != 4
    base_flow = None
    base_manifest = None
    if base is not None:
        base_manifest = load_manifest(base, request.app.state.cipher)
    if base is not None and base.schema_version == 4 and base_manifest is not None:
        manifest = base_manifest
        base_flow = FlowDefinitionV2.model_validate(manifest["flow"])
    result = diff_flows(base_flow, body.configuration.flow)
    configuration_changes = revision_configuration_diff(base_manifest, body.configuration)
    return FlowDiffResult(
        base_revision_id=None if base is None else base.id,
        legacy_base=legacy_base,
        **configuration_changes,
        **result,
    )


@router.post("/draft/publish", response_model=RevisionResponse)
def publish_draft_v4(
    request: Request,
    body: PublishDraftRequestV4 | None = None,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    configuration = load_draft_v4(session, request.app.state.cipher)
    if body is not None:
        if configuration.edit_version != body.edit_version:
            raise HTTPException(
                status_code=409,
                detail="The draft changed after review. Review the current draft before publishing.",
            )
        latest_revision_id = session.scalar(select(Revision.id).order_by(Revision.id.desc()))
        if latest_revision_id != body.base_revision_id:
            raise HTTPException(
                status_code=409,
                detail="The published revision changed after review. Review the draft again.",
            )
    errors = validation_errors(request, session, configuration)
    if errors:
        raise HTTPException(status_code=422, detail=errors)
    _require_v4_device_capabilities(session, configuration)
    revision = create_revision_v4(
        session,
        request.app.state.cipher,
        request.app.state.signer,
        configuration,
        prompts_for_configuration_v4(session, configuration),
        actor,
    )
    session.flush()
    session.execute(
        update(Device).where(Device.revoked_at.is_(None)).values(desired_revision_id=revision.id),
    )
    audit(session, actor, "revision.v4_published", f"revision:{revision.id}")
    session.commit()
    return revision_response(revision)


@router.get("/revisions/{revision_id}", response_model=RevisionDetail)
def revision_detail_v4(
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
        revision=revision_response(revision),
        schema_version=revision.schema_version,
        flow=manifest.get("flow"),
        legacy=revision.schema_version != 4,
    )


@router.post("/revisions/{revision_id}/rollback", response_model=RevisionResponse)
def rollback_v4_revision(
    revision_id: int,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version != 4:
        raise HTTPException(
            status_code=409,
            detail="Use activate-rollback for an immutable V1/V2/V3 revision.",
        )
    manifest = load_manifest(source, request.app.state.cipher)
    current = load_draft_v4(session, request.app.state.cipher)
    configuration = DraftConfigurationV4.model_validate(
        {
            "schema_version": 4,
            "edit_version": current.edit_version,
            "caller_policy": manifest["caller_policy"],
            "schedules": manifest["schedules"],
            "recording_behavior": manifest["recording_behavior"],
            "flow": manifest["flow"],
        },
    )
    errors = validation_errors(request, session, configuration)
    if errors:
        raise HTTPException(status_code=409, detail=errors)
    _require_v4_device_capabilities(session, configuration)
    revision = create_revision_v4(
        session,
        request.app.state.cipher,
        request.app.state.signer,
        configuration,
        prompts_for_configuration_v4(session, configuration),
        actor,
        source_revision_id=source.id,
    )
    store_draft_v4(session, request.app.state.cipher, configuration, actor)
    session.flush()
    session.execute(
        update(Device).where(Device.revoked_at.is_(None)).values(desired_revision_id=revision.id),
    )
    audit(
        session,
        actor,
        "revision.rollback_published",
        f"revision:{revision.id}",
        {"source_revision_id": source.id},
    )
    session.commit()
    return revision_response(revision)


@router.post("/revisions/{revision_id}/activate-rollback", response_model=RevisionResponse)
def activate_immutable_revision(
    revision_id: int,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version not in {1, 2, 3}:
        raise HTTPException(
            status_code=409,
            detail="Only immutable V1/V2/V3 rollback uses this endpoint.",
        )
    manifest = load_manifest(source, request.app.state.cipher)
    _require_immutable_revision_capabilities(session, source, manifest)
    affected = session.execute(
        update(Device).where(Device.revoked_at.is_(None)).values(desired_revision_id=source.id),
    ).rowcount
    audit(
        session,
        actor,
        "revision.legacy_rollback_activated",
        f"revision:{source.id}",
        {"schema_version": source.schema_version, "affected_devices": affected},
    )
    session.commit()
    return revision_response(source)
