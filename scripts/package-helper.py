#!/usr/bin/env python3

from pathlib import Path
import os
import sys
import zipfile


FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def add_file(
    archive: zipfile.ZipFile,
    source: Path,
    destination: str,
    mode: int,
) -> None:
    info = zipfile.ZipInfo(destination, FIXED_TIME)
    info.create_system = 3
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = (0o100000 | mode) << 16
    archive.writestr(info, source.read_bytes())


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: package-helper.py REPOSITORY BINARY OUTPUT",
            file=sys.stderr,
        )
        return 2

    repository = Path(sys.argv[1]).resolve()
    binary = Path(sys.argv[2]).resolve()
    output = Path(sys.argv[3]).resolve()
    entries = (
        (repository / "helper/magisk/module.prop", "module.prop", 0o644),
        (repository / "helper/magisk/service.sh", "service.sh", 0o755),
        (repository / "helper/magisk/disable", "disable", 0o644),
        (binary, "bin/ivrdroid-helper", 0o755),
        (repository / "helper/prompts/main-menu.wav", "prompts/main-menu.wav", 0o644),
        (
            repository / "helper/prompts/sales-unavailable.wav",
            "prompts/sales-unavailable.wav",
            0o644,
        ),
        (
            repository / "helper/prompts/support-unavailable.wav",
            "prompts/support-unavailable.wav",
            0o644,
        ),
        (
            repository / "helper/prompts/operator-unavailable.wav",
            "prompts/operator-unavailable.wav",
            0o644,
        ),
    )

    for source, _, _ in entries:
        if not source.is_file():
            print(f"missing package input: {source}", file=sys.stderr)
            return 1

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    try:
        with zipfile.ZipFile(temporary, "w", allowZip64=False) as archive:
            for source, destination, mode in entries:
                add_file(archive, source, destination, mode)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)

    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
