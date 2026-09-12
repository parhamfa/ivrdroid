#!/usr/bin/env python3

from pathlib import Path
import os
import re
import sys
import zipfile


FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def validate_module_version(repository: Path) -> bool:
    build_config = (repository / "app/build.gradle.kts").read_text()
    module_config = (repository / "app/magisk/module.prop").read_text()
    version_name = re.search(r'^\s*versionName\s*=\s*"([^"]+)"', build_config, re.MULTILINE)
    version_code = re.search(r"^\s*versionCode\s*=\s*(\d+)", build_config, re.MULTILINE)
    module_values = dict(
        line.split("=", 1)
        for line in module_config.splitlines()
        if "=" in line
    )
    if version_name is None or version_code is None:
        print("Android app version is missing from app/build.gradle.kts.", file=sys.stderr)
        return False
    expected = (version_name.group(1), version_code.group(1))
    actual = (module_values.get("version"), module_values.get("versionCode"))
    if actual != expected:
        print(
            "system-app module version does not match the Android app: "
            f"module={actual[0]}/{actual[1]} app={expected[0]}/{expected[1]}",
            file=sys.stderr,
        )
        return False
    return True


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
            "usage: package-system-app.py REPOSITORY APK OUTPUT",
            file=sys.stderr,
        )
        return 2

    repository = Path(sys.argv[1]).resolve()
    apk = Path(sys.argv[2]).resolve()
    output = Path(sys.argv[3]).resolve()
    if not validate_module_version(repository):
        return 1
    entries = (
        (repository / "app/magisk/module.prop", "module.prop", 0o644),
        (repository / "app/magisk/customize.sh", "customize.sh", 0o755),
        (
            repository / "app/magisk/privapp-permissions-ai.rx1.ivrdroid.xml",
            "system/etc/permissions/privapp-permissions-ai.rx1.ivrdroid.xml",
            0o644,
        ),
        (
            repository / "app/magisk/sysconfig-ai.rx1.ivrdroid.xml",
            "system/etc/sysconfig/ai.rx1.ivrdroid.xml",
            0o644,
        ),
        (apk, "system/priv-app/IVRdroid/IVRdroid.apk", 0o644),
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
