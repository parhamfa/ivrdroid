#!/bin/sh

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HELPER_DIR=$(CDPATH= cd -- "$TEST_DIR/.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivrdroid-dtmf-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

"${CXX:-c++}" \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -I"$HELPER_DIR/src" \
    "$HELPER_DIR/src/dtmf_detector.cpp" \
    "$TEST_DIR/dtmf_detector_test.cpp" \
    -o "$BUILD_DIR/dtmf_detector_test"

"$BUILD_DIR/dtmf_detector_test"
