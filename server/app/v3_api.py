from __future__ import annotations

from fastapi import APIRouter, Depends, HTTPException, Request
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from .auth import require_admin, require_admin_write
from .compiler_v3 import compile_flow_v3
from .database import get_session
from .diff_v2 import diff_flows
from .flow_v2 import walk_blocks
from .models import Device, Revision
from .schemas import (
    DraftConfigurationV3,
    FlowDefinitionV2,
    FlowDiffRequestV3,
    FlowDiffResult,
    RecordMessageBlockV3,
    RevisionDetail,
    RevisionResponse,
    SimulationRequestV3,
    SimulationResult,
    ValidationResult,
)
from .services import (
    DraftConflictError,
    audit,
    create_revision_v3,
    load_draft_v3,
    load_manifest,
    prompts_for_configuration_v3,
    store_draft_v3,
)
from .simulation_v3 import simulate_v3
from .validation import validate_configuration_v3


router = APIRouter(prefix="/api/admin/v3", tags=["admin-v3"])


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
    configuration: DraftConfigurationV3,
) -> list[str]:
    prompts = prompts_for_configuration_v3(session, configuration)
    errors = validate_configuration_v3(
        configuration,
        {prompt.id for prompt in prompts},
        {prompt.id: prompt.size_bytes for prompt in prompts},
        request.app.state.settings.revision_asset_limit_bytes,
    )
    if not errors:
        try:
            compile_flow_v3(
                configuration.flow,
                configuration.recording_behavior,
                {prompt.id: prompt.duration_ms for prompt in prompts},
            )
        except (TypeError, ValueError) as error:
            errors.append(str(error))
    return list(dict.fromkeys(errors))


def _require_device_capabilities(session: Session, configuration: DraftConfigurationV3) -> None:
    recording_required = any(
        isinstance(block, RecordMessageBlockV3)
        for block, _ in walk_blocks(configuration.flow.root)
    )
    incompatible: list[str] = []
    for device in session.scalars(select(Device).where(Device.revoked_at.is_(None))):
        status = device.status or {}
        versions = status.get("runtime_versions", [1, 2])
        if 3 not in versions or (recording_required and not status.get("recording_capable", False)):
            incompatible.append(device.display_name)
    if incompatible:
        raise HTTPException(
            status_code=409,
            detail=(
                "V3 cannot be assigned until every active device reports runtime V3"
                + (" and recording capability" if recording_required else "")
                + f": {', '.join(incompatible)}"
            ),
        )


@router.get("/draft", response_model=DraftConfigurationV3)
def get_draft_v3(
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> DraftConfigurationV3:
    return load_draft_v3(session, request.app.state.cipher)


@router.put("/draft", response_model=DraftConfigurationV3)
def put_draft_v3(
    configuration: DraftConfigurationV3,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> DraftConfigurationV3:
    try:
        saved = store_draft_v3(session, request.app.state.cipher, configuration, actor)
    except DraftConflictError as error:
        raise HTTPException(status_code=409, detail=str(error)) from error
    audit(session, actor, "draft.v3_updated", "configuration", {"edit_version": saved.edit_version})
    session.commit()
    return saved


@router.post("/draft/validate", response_model=ValidationResult)
def validate_draft_v3(
    request: Request,
    configuration: DraftConfigurationV3 | None = None,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> ValidationResult:
    candidate = configuration or load_draft_v3(session, request.app.state.cipher)
    errors = validation_errors(request, session, candidate)
    return ValidationResult(valid=not errors, errors=errors)


@router.post("/draft/simulate", response_model=SimulationResult)
def simulate_draft_v3(
    body: SimulationRequestV3,
    request: Request,
    _: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> dict:
    errors = validation_errors(request, session, body.configuration)
    if errors:
        return {"status": "invalid", "trace": [], "message": " ".join(errors)}
    return simulate_v3(body.configuration, body.events)


@router.post("/draft/diff", response_model=FlowDiffResult)
def diff_draft_v3(
    body: FlowDiffRequestV3,
    request: Request,
    _: str = Depends(require_admin),
    session: Session = Depends(get_session),
) -> FlowDiffResult:
    base = session.get(Revision, body.base_revision_id) if body.base_revision_id else session.scalar(
        select(Revision).where(Revision.schema_version == 3).order_by(Revision.id.desc()),
    )
    legacy_base = False
    base_flow = None
    changes: list[str] = []
    if base is not None and base.schema_version == 3:
        manifest = load_manifest(base, request.app.state.cipher)
        base_flow = FlowDefinitionV2.model_validate(manifest["flow"])
        if manifest.get("recording_behavior") != body.configuration.recording_behavior.model_dump(mode="json"):
            changes.append("Changed shared recording behavior")
    elif base is not None or session.scalar(select(Revision.id).order_by(Revision.id.desc())) is not None:
        legacy_base = True
    result = diff_flows(base_flow, body.configuration.flow)
    result["changed"] += len(changes)
    result["changes"] = [*changes, *result["changes"]][:256]
    return FlowDiffResult(
        base_revision_id=None if base is None else base.id,
        legacy_base=legacy_base,
        **result,
    )


@router.post("/draft/publish", response_model=RevisionResponse)
def publish_draft_v3(
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    configuration = load_draft_v3(session, request.app.state.cipher)
    errors = validation_errors(request, session, configuration)
    if errors:
        raise HTTPException(status_code=422, detail=errors)
    _require_device_capabilities(session, configuration)
    revision = create_revision_v3(
        session,
        request.app.state.cipher,
        request.app.state.signer,
        configuration,
        prompts_for_configuration_v3(session, configuration),
        actor,
    )
    session.flush()
    session.execute(
        update(Device).where(Device.revoked_at.is_(None)).values(desired_revision_id=revision.id),
    )
    audit(session, actor, "revision.v3_published", f"revision:{revision.id}")
    session.commit()
    return revision_response(revision)


@router.get("/revisions/{revision_id}", response_model=RevisionDetail)
def revision_detail_v3(
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
        legacy=revision.schema_version != 3,
    )


@router.post("/revisions/{revision_id}/rollback", response_model=RevisionResponse)
def rollback_v3_revision(
    revision_id: int,
    request: Request,
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version != 3:
        raise HTTPException(
            status_code=409,
            detail="Use activate-rollback for an immutable V1/V2 revision.",
        )
    manifest = load_manifest(source, request.app.state.cipher)
    current = load_draft_v3(session, request.app.state.cipher)
    configuration = DraftConfigurationV3.model_validate(
        {
            "schema_version": 3,
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
    _require_device_capabilities(session, configuration)
    revision = create_revision_v3(
        session,
        request.app.state.cipher,
        request.app.state.signer,
        configuration,
        prompts_for_configuration_v3(session, configuration),
        actor,
        source_revision_id=source.id,
    )
    store_draft_v3(session, request.app.state.cipher, configuration, actor)
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
    actor: str = Depends(require_admin_write),
    session: Session = Depends(get_session),
) -> RevisionResponse:
    source = session.get(Revision, revision_id)
    if source is None:
        raise HTTPException(status_code=404, detail="Revision not found")
    if source.schema_version not in {1, 2}:
        raise HTTPException(status_code=409, detail="Only immutable V1/V2 rollback uses this endpoint.")
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
