#!/bin/sh

set -eu

TEST_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
HELPER_DIR=$(CDPATH='' cd -- "$TEST_DIR/.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ivrdroid-helper-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

compile_test() {
    output=$1
    shift
    "${CXX:-c++}" \
        -std=c++17 \
        -Wall \
        -Wextra \
        -Wpedantic \
        -Werror \
        -I"$HELPER_DIR/src" \
        "$@" \
        -o "$BUILD_DIR/$output"
}

compile_test \
    dtmf_detector_test \
    "$HELPER_DIR/src/dtmf_detector.cpp" \
    "$TEST_DIR/dtmf_detector_test.cpp"
compile_test \
    menu_policy_test \
    "$HELPER_DIR/src/menu_policy.cpp" \
    "$TEST_DIR/menu_policy_test.cpp"
compile_test \
    protocol_test \
    "$HELPER_DIR/src/helper_protocol.cpp" \
    "$TEST_DIR/protocol_test.cpp"
compile_test \
    telecom_guard_test \
    "$HELPER_DIR/src/telecom_guard.cpp" \
    "$TEST_DIR/telecom_guard_test.cpp"
compile_test \
    device_profile_test \
    "$HELPER_DIR/src/device_profile.cpp" \
    "$TEST_DIR/device_profile_test.cpp"

"$BUILD_DIR/dtmf_detector_test"
"$BUILD_DIR/menu_policy_test"
"$BUILD_DIR/protocol_test"
"$BUILD_DIR/telecom_guard_test"
"$BUILD_DIR/device_profile_test"
