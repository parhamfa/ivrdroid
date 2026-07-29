#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivrdroid-helper-repro.XXXXXX")
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

mkdir -p "$TEMP_DIR/source-a" "$TEMP_DIR/source-b"
rsync -a \
    --exclude 'build-android-*' \
    --exclude 'dist' \
    "$REPO_DIR/helper/" \
    "$TEMP_DIR/source-a/"
rsync -a \
    --exclude 'build-android-*' \
    --exclude 'dist' \
    "$REPO_DIR/helper/" \
    "$TEMP_DIR/source-b/"

"$REPO_DIR/scripts/build-helper.sh" \
    "$TEMP_DIR/build-a" \
    "$TEMP_DIR/source-a" >/dev/null
"$REPO_DIR/scripts/build-helper.sh" \
    "$TEMP_DIR/build-b" \
    "$TEMP_DIR/source-b" >/dev/null

cmp "$TEMP_DIR/build-a/ivrdroid-helper" "$TEMP_DIR/build-b/ivrdroid-helper"
shasum -a 256 "$TEMP_DIR/build-a/ivrdroid-helper"
echo "Helper build is reproducible across two source and build paths."
