"""The PCM workspace belongs inside the same private, application-owned volume."""
import os
import stat
import tempfile
from pathlib import Path


def processing_workspace(recording_root: Path, *, probe: bool = False) -> Path:
    root = recording_root.lstat()
    if not stat.S_ISDIR(root.st_mode) or root.st_uid != os.geteuid() or root.st_mode & 0o077:
        raise PermissionError("Recording volume must be a private directory owned by the application UID")
    path = recording_root / ".processing"
    path.mkdir(mode=0o700, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise PermissionError("Processing workspace must be a private directory owned by the application UID")
    if probe:
        fd, name = tempfile.mkstemp(prefix=".permission-probe-", dir=path)
        try:
            os.write(fd, b"ivrdroid processing permission probe\n")
            os.fsync(fd)
        finally:
            os.close(fd)
            os.unlink(name)
        directory = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return path


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    print(f"uid={os.geteuid()} workspace={processing_workspace(args.root, probe=True)} writable and fsynced")
