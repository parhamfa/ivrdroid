#!/bin/sh

set -eu

REPO_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${1:-"$REPO_DIR/helper/build-android-arm64"}
SOURCE_DIR=${2:-"$REPO_DIR/helper"}
NDK_VERSION=25.2.9519653

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$REPO_DIR/$BUILD_DIR" ;;
esac
case "$SOURCE_DIR" in
    /*) ;;
    *) SOURCE_DIR="$REPO_DIR/$SOURCE_DIR" ;;
esac

SDK_DIR=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$SDK_DIR" ] && [ -f "$REPO_DIR/local.properties" ]; then
    SDK_DIR=$(sed -n 's/^sdk\.dir=//p' "$REPO_DIR/local.properties" | head -n 1)
fi
if [ -z "$SDK_DIR" ] || [ ! -d "$SDK_DIR/ndk/$NDK_VERSION" ]; then
    echo "Android NDK $NDK_VERSION was not found. Set ANDROID_SDK_ROOT." >&2
    exit 1
fi

TOOLCHAIN="$SDK_DIR/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake"
cmake \
    -Wno-deprecated \
    -S "$SOURCE_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-23 \
    -DIVRDROID_SOURCE_COMMIT="${IVRDROID_SOURCE_COMMIT:-unknown}" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

test -x "$BUILD_DIR/ivrdroid-helper"
printf '%s\n' "$BUILD_DIR/ivrdroid-helper"
