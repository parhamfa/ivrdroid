#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
APK="$REPO_DIR/app/build/outputs/apk/debug/app-debug.apk"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivrdroid-system-app-repro.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM

if [ ! -f "$APK" ]; then
    echo "missing debug APK: $APK" >&2
    exit 1
fi

python3 "$REPO_DIR/scripts/package-system-app.py" \
    "$REPO_DIR" "$APK" "$WORK_DIR/first.zip" >/dev/null
python3 "$REPO_DIR/scripts/package-system-app.py" \
    "$REPO_DIR" "$APK" "$WORK_DIR/second.zip" >/dev/null

cmp "$WORK_DIR/first.zip" "$WORK_DIR/second.zip"
unzip -t "$WORK_DIR/first.zip" >/dev/null
shasum -a 256 "$WORK_DIR/first.zip"
