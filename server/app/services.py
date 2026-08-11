from __future__ import annotations

import json
from datetime import datetime, timezone
from uuid import uuid4

from sqlalchemy import select, update
from sqlalchemy.orm import Session

from .compiler_v2 import compile_flow
from .compiler_v3 import compile_flow_v3
from .compiler_v4 import compile_flow_v4
from .crypto import DataCipher, RevisionSigner, canonical_json_bytes
from .flow_v2 import prompt_ids, source_document
from .models import AuditLog, Draft, LegacyDraftArchive, Prompt, Revision
from .schemas import (
    DraftConfiguration,
    DraftConfigurationV2,
    DraftConfigurationV3,
    DraftConfigurationV4,
)


class DraftConflictError(RuntimeError):
    pass


V4_DRAFT_ID = 4


def load_draft(session: Session, cipher: DataCipher) -> DraftConfiguration:
    draft = session.get(Draft, 1)
    if draft is None:
        return DraftConfiguration()
    return DraftConfiguration.model_validate(json.loads(cipher.decrypt(draft.document_encrypted)))


def store_draft(
    session: Session,
    cipher: DataCipher,
    configuration: DraftConfiguration,
    actor: str,
) -> Draft:
    document = configuration.model_dump(mode="json")
    encrypted = cipher.encrypt(canonical_json_bytes(document).decode("utf-8"))
    draft = session.get(Draft, 1)
    if draft is None:
        draft = Draft(id=1, document_encrypted=encrypted, edit_version=1, updated_by=actor)
        session.add(draft)
    else:
        draft.document_encrypted = encrypted
        draft.edit_version += 1
        draft.updated_by = actor
        draft.updated_at = datetime.now(timezone.utc)
    return draft


def load_draft_v2(session: Session, cipher: DataCipher) -> DraftConfigurationV2:
    draft = session.get(Draft, 1)
    if draft is None:
        return DraftConfigurationV2()
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") == 2:
        document["edit_version"] = draft.edit_version
        return DraftConfigurationV2.model_validate(document)
    legacy = DraftConfiguration.model_validate(document)
    return DraftConfigurationV2(
        edit_version=draft.edit_version,
        caller_policy=legacy.caller_policy,
        schedules=legacy.schedules,
    )


def initialize_v2_draft(
    session: Session,
    cipher: DataCipher,
    actor: str,
) -> DraftConfigurationV2:
    """Persist a blank V2 tree while retaining non-flow control settings."""
    draft = session.get(Draft, 1)
    if draft is None:
        return store_draft_v2(session, cipher, DraftConfigurationV2(), actor)
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") == 2:
        return load_draft_v2(session, cipher)
    legacy = DraftConfiguration.model_validate(document)
    return store_draft_v2(
        session,
        cipher,
        DraftConfigurationV2(
            edit_version=draft.edit_version,
            caller_policy=legacy.caller_policy,
            schedules=legacy.schedules,
        ),
        actor,
    )


def store_draft_v2(
    session: Session,
    cipher: DataCipher,
    configuration: DraftConfigurationV2,
    actor: str,
) -> DraftConfigurationV2:
    draft = session.scalar(select(Draft).where(Draft.id == 1).with_for_update())
    current_version = 0 if draft is None else draft.edit_version
    if configuration.edit_version != current_version:
        raise DraftConflictError(
            f"Draft changed from version {configuration.edit_version} to {current_version}.",
        )
    document = source_document(configuration)
    encrypted = cipher.encrypt(canonical_json_bytes(document).decode("utf-8"))
    next_version = current_version + 1
    if draft is None:
        draft = Draft(
            id=1,
            document_encrypted=encrypted,
            edit_version=next_version,
            updated_by=actor,
        )
        session.add(draft)
    else:
        previous = json.loads(cipher.decrypt(draft.document_encrypted))
        if previous.get("schema_version") == 1:
            already_archived = session.scalar(select(LegacyDraftArchive.id).limit(1))
            if already_archived is None:
                session.add(
                    LegacyDraftArchive(
                        schema_version=1,
                        document_encrypted=draft.document_encrypted,
                        archived_by=draft.updated_by,
                    ),
                )
        result = session.execute(
            update(Draft)
            .where(Draft.id == 1, Draft.edit_version == current_version)
            .values(
                document_encrypted=encrypted,
                edit_version=next_version,
                updated_by=actor,
                updated_at=datetime.now(timezone.utc),
            ),
        )
        if result.rowcount != 1:
            session.rollback()
            raise DraftConflictError("Draft changed while this save was being applied.")
    return configuration.model_copy(update={"edit_version": next_version})


def load_draft_v3(session: Session, cipher: DataCipher) -> DraftConfigurationV3:
    draft = session.get(Draft, 1)
    if draft is None:
        return DraftConfigurationV3()
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") != 3:
        raise RuntimeError("The editable draft has not been migrated to V3.")
    document["edit_version"] = draft.edit_version
    return DraftConfigurationV3.model_validate(document)


def initialize_v3_draft(
    session: Session,
    cipher: DataCipher,
    actor: str,
) -> DraftConfigurationV3:
    """Archive the editable V1/V2 draft and promote a V3-owned tree exactly once."""
    draft = session.get(Draft, 1)
    if draft is None:
        return store_draft_v3(session, cipher, DraftConfigurationV3(), actor)
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") == 3:
        return load_draft_v3(session, cipher)

    schema_version = int(document.get("schema_version", 1))
    session.add(
        LegacyDraftArchive(
            schema_version=schema_version,
            document_encrypted=draft.document_encrypted,
            archived_by=draft.updated_by,
        ),
    )
    if schema_version == 2:
        previous = DraftConfigurationV2.model_validate(
            {**document, "edit_version": draft.edit_version},
        )
        promoted = DraftConfigurationV3(
            edit_version=draft.edit_version,
            caller_policy=previous.caller_policy,
            schedules=previous.schedules,
            flow=previous.flow,
        )
    else:
        previous_v1 = DraftConfiguration.model_validate(document)
        promoted = DraftConfigurationV3(
            edit_version=draft.edit_version,
            caller_policy=previous_v1.caller_policy,
            schedules=previous_v1.schedules,
        )
    return store_draft_v3(session, cipher, promoted, actor)


def store_draft_v3(
    session: Session,
    cipher: DataCipher,
    configuration: DraftConfigurationV3,
    actor: str,
) -> DraftConfigurationV3:
    draft = session.scalar(select(Draft).where(Draft.id == 1).with_for_update())
    current_version = 0 if draft is None else draft.edit_version
    if configuration.edit_version != current_version:
        raise DraftConflictError(
            f"Draft changed from version {configuration.edit_version} to {current_version}.",
        )
    document = configuration.model_dump(mode="json", exclude={"edit_version"})
    encrypted = cipher.encrypt(canonical_json_bytes(document).decode("utf-8"))
    next_version = current_version + 1
    if draft is None:
        session.add(
            Draft(
                id=1,
                document_encrypted=encrypted,
                edit_version=next_version,
                updated_by=actor,
            ),
        )
    else:
        result = session.execute(
            update(Draft)
            .where(Draft.id == 1, Draft.edit_version == current_version)
            .values(
                document_encrypted=encrypted,
                edit_version=next_version,
                updated_by=actor,
                updated_at=datetime.now(timezone.utc),
            ),
        )
        if result.rowcount != 1:
            session.rollback()
            raise DraftConflictError("Draft changed while this save was being applied.")
    return configuration.model_copy(update={"edit_version": next_version})


def load_draft_v4(session: Session, cipher: DataCipher) -> DraftConfigurationV4:
    draft = session.get(Draft, V4_DRAFT_ID)
    if draft is None:
        return DraftConfigurationV4()
    document = json.loads(cipher.decrypt(draft.document_encrypted))
    if document.get("schema_version") != 4:
        raise RuntimeError("The editable draft has not been cloned to V4.")
    document["edit_version"] = draft.edit_version
    return DraftConfigurationV4.model_validate(document)


def initialize_v4_draft(
    session: Session,
    cipher: DataCipher,
    actor: str,
) -> DraftConfigurationV4:
    """Clone the live V1/V2/V3 draft once without mutating its authoring lane."""
    v4_draft = session.get(Draft, V4_DRAFT_ID)
    if v4_draft is not None:
        return load_draft_v4(session, cipher)

    source = session.get(Draft, 1)
    if source is None:
        return store_draft_v4(session, cipher, DraftConfigurationV4(), actor)
    document = json.loads(cipher.decrypt(source.document_encrypted))
    schema_version = int(document.get("schema_version", 1))
    if schema_version == 3:
        previous_v3 = DraftConfigurationV3.model_validate(
            {**document, "edit_version": source.edit_version},
        )
        promoted = DraftConfigurationV4(
            caller_policy=previous_v3.caller_policy,
            schedules=previous_v3.schedules,
            recording_behavior=previous_v3.recording_behavior,
            flow=previous_v3.flow,
        )
    elif schema_version == 2:
        previous_v2 = DraftConfigurationV2.model_validate(
            {**document, "edit_version": source.edit_version},
        )
        promoted = DraftConfigurationV4(
            caller_policy=previous_v2.caller_policy,
            schedules=previous_v2.schedules,
            flow=previous_v2.flow,
        )
    else:
        previous_v1 = DraftConfiguration.model_validate(document)
        promoted = DraftConfigurationV4(
            caller_policy=previous_v1.caller_policy,
            schedules=previous_v1.schedules,
        )
    return store_draft_v4(session, cipher, promoted, actor)


def store_draft_v4(
    session: Session,
    cipher: DataCipher,
    configuration: DraftConfigurationV4,
    actor: str,
) -> DraftConfigurationV4:
    draft = session.scalar(select(Draft).where(Draft.id == V4_DRAFT_ID).with_for_update())
    current_version = 0 if draft is None else draft.edit_version
    if configuration.edit_version != current_version:
        raise DraftConflictError(
            f"Draft changed from version {configuration.edit_version} to {current_version}.",
        )
    document = configuration.model_dump(mode="json", exclude={"edit_version"})
    encrypted = cipher.encrypt(canonical_json_bytes(document).decode("utf-8"))
    next_version = current_version + 1
    if draft is None:
        session.add(
            Draft(
                id=V4_DRAFT_ID,
                document_encrypted=encrypted,
                edit_version=next_version,
                updated_by=actor,
            ),
        )
    else:
        result = session.execute(
            update(Draft)
            .where(Draft.id == V4_DRAFT_ID, Draft.edit_version == current_version)
            .values(
                document_encrypted=encrypted,
                edit_version=next_version,
                updated_by=actor,
                updated_at=datetime.now(timezone.utc),
            ),
        )
        if result.rowcount != 1:
            session.rollback()
            raise DraftConflictError("Draft changed while this save was being applied.")
    return configuration.model_copy(update={"edit_version": next_version})


def build_manifest(
    revision_id: int,
    configuration: DraftConfiguration,
    prompts: list[Prompt],
    published_at: datetime,
    rolled_back_from: int | None = None,
) -> dict:
    prompt_documents = [
        {
            "id": prompt.id,
            "name": prompt.name,
            "version": prompt.version,
            "content_hash": prompt.content_hash,
            "size_bytes": prompt.size_bytes,
            "duration_ms": prompt.duration_ms,
            "format": "audio/wav;codec=pcm_s16le;rate=48000;channels=2",
        }
        for prompt in sorted(prompts, key=lambda item: item.id)
    ]
    manifest = {
        "schema_version": 1,
        "revision_id": revision_id,
        "published_at": published_at.isoformat(),
        "caller_policy": configuration.caller_policy.model_dump(mode="json"),
        "schedules": [item.model_dump(mode="json") for item in configuration.schedules],
        "flow": configuration.flow.model_dump(mode="json"),
        "prompts": prompt_documents,
    }
    if rolled_back_from is not None:
        manifest["rolled_back_from"] = rolled_back_from
    return manifest


def create_revision(
    session: Session,
    cipher: DataCipher,
    signer: RevisionSigner,
    configuration: DraftConfiguration,
    prompts: list[Prompt],
    actor: str,
    source_revision_id: int | None = None,
) -> Revision:
    published_at = datetime.now(timezone.utc)
    revision = Revision(
        schema_version=1,
        manifest_encrypted=cipher.encrypt("{}"),
        manifest_sha256=f"pending-{uuid4().hex}",
        signature_b64="pending",
        source_revision_id=source_revision_id,
        published_at=published_at,
        published_by=actor,
    )
    session.add(revision)
    session.flush()
    manifest = build_manifest(
        revision.id,
        configuration,
        prompts,
        published_at,
        rolled_back_from=source_revision_id,
    )
    digest, signature = signer.sign(manifest)
    revision.manifest_encrypted = cipher.encrypt(canonical_json_bytes(manifest).decode("utf-8"))
    revision.manifest_sha256 = digest
    revision.signature_b64 = signature
    return revision


def build_manifest_v2(
    revision_id: int,
    configuration: DraftConfigurationV2,
    prompts: list[Prompt],
    published_at: datetime,
    rolled_back_from: int | None = None,
) -> dict:
    compiled = compile_flow(configuration.flow)
    manifest = {
        "schema_version": 2,
        "revision_id": revision_id,
        "published_at": published_at.isoformat(),
        "caller_policy": configuration.caller_policy.model_dump(mode="json"),
        "schedules": [item.model_dump(mode="json") for item in configuration.schedules],
        "flow": configuration.flow.model_dump(mode="json"),
        "compiler_version": compiled["compiler_version"],
        "source_sha256": compiled["source_sha256"],
        "program_sha256": compiled["program_sha256"],
        "program": compiled["program"],
        "prompts": [
            {
                "id": prompt.id,
                "name": prompt.name,
                "version": prompt.version,
                "content_hash": prompt.content_hash,
                "size_bytes": prompt.size_bytes,
                "duration_ms": prompt.duration_ms,
                "format": "audio/wav;codec=pcm_s16le;rate=48000;channels=2",
            }
            for prompt in sorted(prompts, key=lambda item: item.id)
        ],
    }
    if rolled_back_from is not None:
        manifest["rolled_back_from"] = rolled_back_from
    return manifest


def create_revision_v2(
    session: Session,
    cipher: DataCipher,
    signer: RevisionSigner,
    configuration: DraftConfigurationV2,
    prompts: list[Prompt],
    actor: str,
    source_revision_id: int | None = None,
) -> Revision:
    published_at = datetime.now(timezone.utc)
    revision = Revision(
        schema_version=2,
        manifest_encrypted=cipher.encrypt("{}"),
        manifest_sha256=f"pending-{uuid4().hex}",
        signature_b64="pending",
        source_revision_id=source_revision_id,
        published_at=published_at,
        published_by=actor,
    )
    session.add(revision)
    session.flush()
    manifest = build_manifest_v2(
        revision.id,
        configuration,
        prompts,
        published_at,
        rolled_back_from=source_revision_id,
    )
    digest, signature = signer.sign(manifest)
    revision.manifest_encrypted = cipher.encrypt(canonical_json_bytes(manifest).decode("utf-8"))
    revision.manifest_sha256 = digest
    revision.signature_b64 = signature
    return revision


def build_manifest_v3(
    revision_id: int,
    configuration: DraftConfigurationV3,
    prompts: list[Prompt],
    published_at: datetime,
    rolled_back_from: int | None = None,
) -> dict:
    compiled = compile_flow_v3(
        configuration.flow,
        configuration.recording_behavior,
        {prompt.id: prompt.duration_ms for prompt in prompts},
    )
    manifest = {
        "schema_version": 3,
        "revision_id": revision_id,
        "published_at": published_at.isoformat(),
        "caller_policy": configuration.caller_policy.model_dump(mode="json"),
        "schedules": [item.model_dump(mode="json") for item in configuration.schedules],
        "recording_behavior": configuration.recording_behavior.model_dump(mode="json"),
        "flow": configuration.flow.model_dump(mode="json"),
        "compiler_version": compiled["compiler_version"],
        "source_sha256": compiled["source_sha256"],
        "program_sha256": compiled["program_sha256"],
        "program": compiled["program"],
        "prompts": [
            {
                "id": prompt.id,
                "name": prompt.name,
                "version": prompt.version,
                "content_hash": prompt.content_hash,
                "size_bytes": prompt.size_bytes,
                "duration_ms": prompt.duration_ms,
                "format": "audio/wav;codec=pcm_s16le;rate=48000;channels=2",
            }
            for prompt in sorted(prompts, key=lambda item: item.id)
        ],
    }
    if rolled_back_from is not None:
        manifest["rolled_back_from"] = rolled_back_from
    return manifest


def create_revision_v3(
    session: Session,
    cipher: DataCipher,
    signer: RevisionSigner,
    configuration: DraftConfigurationV3,
    prompts: list[Prompt],
    actor: str,
    source_revision_id: int | None = None,
) -> Revision:
    published_at = datetime.now(timezone.utc)
    revision = Revision(
        schema_version=3,
        manifest_encrypted=cipher.encrypt("{}"),
        manifest_sha256=f"pending-{uuid4().hex}",
        signature_b64="pending",
        source_revision_id=source_revision_id,
        published_at=published_at,
        published_by=actor,
    )
    session.add(revision)
    session.flush()
    manifest = build_manifest_v3(
        revision.id,
        configuration,
        prompts,
        published_at,
        rolled_back_from=source_revision_id,
    )
    digest, signature = signer.sign(manifest)
    revision.manifest_encrypted = cipher.encrypt(canonical_json_bytes(manifest).decode("utf-8"))
    revision.manifest_sha256 = digest
    revision.signature_b64 = signature
    return revision


def build_manifest_v4(
    revision_id: int,
    configuration: DraftConfigurationV4,
    prompts: list[Prompt],
    published_at: datetime,
    rolled_back_from: int | None = None,
) -> dict:
    compiled = compile_flow_v4(
        configuration.flow,
        configuration.recording_behavior,
        {prompt.id: prompt.duration_ms for prompt in prompts},
    )
    manifest = {
        "schema_version": 4,
        "revision_id": revision_id,
        "published_at": published_at.isoformat(),
        "caller_policy": configuration.caller_policy.model_dump(mode="json"),
        "schedules": [item.model_dump(mode="json") for item in configuration.schedules],
        "recording_behavior": configuration.recording_behavior.model_dump(mode="json"),
        "flow": configuration.flow.model_dump(mode="json"),
        "compiler_version": compiled["compiler_version"],
        "source_sha256": compiled["source_sha256"],
        "program_sha256": compiled["program_sha256"],
        "program": compiled["program"],
        "prompts": [
            {
                "id": prompt.id,
                "name": prompt.name,
                "version": prompt.version,
                "content_hash": prompt.content_hash,
                "size_bytes": prompt.size_bytes,
                "duration_ms": prompt.duration_ms,
                "format": "audio/wav;codec=pcm_s16le;rate=48000;channels=2",
            }
            for prompt in sorted(prompts, key=lambda item: item.id)
        ],
    }
    if rolled_back_from is not None:
        manifest["rolled_back_from"] = rolled_back_from
    return manifest


def create_revision_v4(
    session: Session,
    cipher: DataCipher,
    signer: RevisionSigner,
    configuration: DraftConfigurationV4,
    prompts: list[Prompt],
    actor: str,
    source_revision_id: int | None = None,
) -> Revision:
    published_at = datetime.now(timezone.utc)
    revision = Revision(
        schema_version=4,
        manifest_encrypted=cipher.encrypt("{}"),
        manifest_sha256=f"pending-{uuid4().hex}",
        signature_b64="pending",
        source_revision_id=source_revision_id,
        published_at=published_at,
        published_by=actor,
    )
    session.add(revision)
    session.flush()
    manifest = build_manifest_v4(
        revision.id,
        configuration,
        prompts,
        published_at,
        rolled_back_from=source_revision_id,
    )
    digest, signature = signer.sign(manifest)
    revision.manifest_encrypted = cipher.encrypt(canonical_json_bytes(manifest).decode("utf-8"))
    revision.manifest_sha256 = digest
    revision.signature_b64 = signature
    return revision


def load_manifest(revision: Revision, cipher: DataCipher) -> dict:
    return json.loads(cipher.decrypt(revision.manifest_encrypted))


def prompts_for_configuration(session: Session, configuration: DraftConfiguration) -> list[Prompt]:
    prompt_ids: set[str] = set()
    for node in configuration.flow.nodes:
        prompt_id = getattr(node, "prompt_id", None)
        if prompt_id:
            prompt_ids.add(prompt_id)
    if not prompt_ids:
        return []
    return list(session.scalars(select(Prompt).where(Prompt.id.in_(prompt_ids))))


def prompts_for_configuration_v2(
    session: Session,
    configuration: DraftConfigurationV2,
) -> list[Prompt]:
    identifiers = prompt_ids(configuration)
    if not identifiers:
        return []
    return list(session.scalars(select(Prompt).where(Prompt.id.in_(identifiers))))


def prompts_for_configuration_v3(
    session: Session,
    configuration: DraftConfigurationV3,
) -> list[Prompt]:
    identifiers = prompt_ids(configuration)
    if not identifiers:
        return []
    return list(session.scalars(select(Prompt).where(Prompt.id.in_(identifiers))))


def prompts_for_configuration_v4(
    session: Session,
    configuration: DraftConfigurationV4,
) -> list[Prompt]:
    identifiers = prompt_ids(configuration)
    if not identifiers:
        return []
    return list(session.scalars(select(Prompt).where(Prompt.id.in_(identifiers))))


def audit(
    session: Session,
    actor: str,
    action: str,
    target: str,
    details: dict | None = None,
) -> None:
    session.add(
        AuditLog(
            actor=actor,
            action=action,
            target=target,
            details=details or {},
        ),
    )
