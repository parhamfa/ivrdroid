import os
from pathlib import Path

import pytest

from app.processing_workspace import processing_workspace
from app.recording_service import RecordingError, reconcile_recording_storage


def test_workspace_is_private_and_storage_maintenance_leaves_it_owned_by_worker(client):
    settings = client.app.state.settings
    workspace = processing_workspace(settings.recording_root, probe=True)
    assert workspace.parent == settings.recording_root
    old_job = workspace / "job-interrupted"
    old_job.mkdir(mode=0o700)
    (old_job / "audio.pcm").write_bytes(b"preserved")
    with client.app.state.database.session() as session:
        reconcile_recording_storage(session, settings)
    assert (old_job / "audio.pcm").read_bytes() == b"preserved"
    workspace.chmod(0o755)
    with pytest.raises(PermissionError):
        processing_workspace(settings.recording_root)
    with client.app.state.database.session() as session, pytest.raises(RecordingError):
        reconcile_recording_storage(session, settings)


@pytest.mark.skipif(os.environ.get("IVRDROID_CONTAINER_PERMISSION_TEST") != "1", reason="Requires the actual image UID and /data layout")
def test_actual_application_uid_can_process_but_cannot_create_sibling_directory():
    assert os.geteuid() == 10001
    assert Path("/data").stat().st_uid == 0
    with pytest.raises(PermissionError):
        Path("/data/recordings-processing").mkdir()
    assert processing_workspace(Path("/data/recordings"), probe=True) == Path("/data/recordings/.processing")
