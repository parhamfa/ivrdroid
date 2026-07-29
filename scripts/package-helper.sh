#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$REPO_DIR/helper/build-android-arm64"
DIST_DIR="$REPO_DIR/helper/dist"
PACKAGE="$DIST_DIR/IVRdroid-helper-0.3.3-dev-disabled.zip"
STRIPPED_BINARY="$DIST_DIR/ivrdroid-helper"
NDK_VERSION=25.2.9519653

"$REPO_DIR/scripts/build-helper.sh" "$BUILD_DIR" >/dev/null

SDK_DIR=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$SDK_DIR" ] && [ -f "$REPO_DIR/local.properties" ]; then
    SDK_DIR=$(sed -n 's/^sdk\.dir=//p' "$REPO_DIR/local.properties" | head -n 1)
fi
STRIP_TOOL=$(find \
    "$SDK_DIR/ndk/$NDK_VERSION/toolchains/llvm/prebuilt" \
    -path '*/bin/llvm-strip' \
    -print \
    -quit)
if [ -z "$STRIP_TOOL" ] || [ ! -x "$STRIP_TOOL" ]; then
    echo "Pinned NDK llvm-strip was not found." >&2
    exit 1
fi

mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/ivrdroid-helper" "$STRIPPED_BINARY"
"$STRIP_TOOL" --strip-unneeded "$STRIPPED_BINARY"
chmod 0755 "$STRIPPED_BINARY"

python3 "$REPO_DIR/scripts/package-helper.py" \
    "$REPO_DIR" \
    "$STRIPPED_BINARY" \
    "$PACKAGE"
unzip -t "$PACKAGE" >/dev/null
shasum -a 256 "$STRIPPED_BINARY" "$PACKAGE"
