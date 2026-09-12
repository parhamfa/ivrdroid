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
    audit_policy_test \
    "$HELPER_DIR/src/audit_policy.cpp" \
    "$TEST_DIR/audit_policy_test.cpp"
compile_test \
    call_control_protocol_test \
    "$HELPER_DIR/src/call_control_protocol.cpp" \
    "$TEST_DIR/call_control_protocol_test.cpp"
compile_test \
    call_safety_policy_test \
    "$HELPER_DIR/src/call_safety_policy.cpp" \
    "$TEST_DIR/call_safety_policy_test.cpp"
compile_test \
    child_process_test \
    "$HELPER_DIR/src/child_process.cpp" \
    "$TEST_DIR/child_process_test.cpp"
compile_test \
    conversation_handoff_protocol_test \
    "$HELPER_DIR/src/call_control_protocol.cpp" \
    "$HELPER_DIR/src/conversation_handoff_protocol.cpp" \
    "$HELPER_DIR/src/recording_policy.cpp" \
    "$TEST_DIR/conversation_handoff_protocol_test.cpp"
compile_test \
    dtmf_detector_test \
    "$HELPER_DIR/src/dtmf_detector.cpp" \
    "$TEST_DIR/dtmf_detector_test.cpp"
compile_test \
    external_call_policy_test \
    "$HELPER_DIR/src/call_control_protocol.cpp" \
    "$HELPER_DIR/src/external_call_policy.cpp" \
    "$TEST_DIR/external_call_policy_test.cpp"
compile_test \
    menu_policy_test \
    "$HELPER_DIR/src/menu_policy.cpp" \
    "$TEST_DIR/menu_policy_test.cpp"
compile_test \
    mixer_route_policy_test \
    "$HELPER_DIR/src/mixer_route_policy.cpp" \
    "$TEST_DIR/mixer_route_policy_test.cpp"
compile_test \
    protocol_test \
    "$HELPER_DIR/src/helper_protocol.cpp" \
    "$TEST_DIR/protocol_test.cpp"
compile_test \
    privacy_policy_test \
    "$HELPER_DIR/src/privacy_policy.cpp" \
    "$TEST_DIR/privacy_policy_test.cpp"
compile_test \
    prompt_barge_in_policy_test \
    "$HELPER_DIR/src/prompt_barge_in_policy.cpp" \
    "$TEST_DIR/prompt_barge_in_policy_test.cpp"
compile_test \
    recording_policy_test \
    "$HELPER_DIR/src/call_control_protocol.cpp" \
    "$HELPER_DIR/src/recording_policy.cpp" \
    "$TEST_DIR/recording_policy_test.cpp"
compile_test \
    sha256_test \
    "$HELPER_DIR/src/sha256.cpp" \
    "$TEST_DIR/sha256_test.cpp"
compile_test \
    revision_config_test \
    "$HELPER_DIR/src/call_control_protocol.cpp" \
    "$HELPER_DIR/src/sha256.cpp" \
    "$HELPER_DIR/src/revision_config.cpp" \
    "$TEST_DIR/revision_config_test.cpp"
compile_test \
    session_snapshot_test \
    "$HELPER_DIR/src/session_snapshot.cpp" \
    "$TEST_DIR/session_snapshot_test.cpp"
compile_test \
    telecom_guard_test \
    "$HELPER_DIR/src/telecom_guard.cpp" \
    "$TEST_DIR/telecom_guard_test.cpp"
compile_test \
    device_profile_test \
    "$HELPER_DIR/src/device_profile.cpp" \
    "$TEST_DIR/device_profile_test.cpp"

"$BUILD_DIR/audit_policy_test"
"$BUILD_DIR/call_control_protocol_test" \
    "$HELPER_DIR/../app/src/test/resources/call_control/dial_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/recorder_ready_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/cancel_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/status_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/answer_timeout_cancel_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/answer_timeout_not_connected_v1.txt"
"$BUILD_DIR/call_safety_policy_test"
"$BUILD_DIR/child_process_test"
"$BUILD_DIR/conversation_handoff_protocol_test" \
    "$HELPER_DIR/../app/src/test/resources/call_control/conversation_handoff_v1.txt"
"$BUILD_DIR/dtmf_detector_test"
"$BUILD_DIR/external_call_policy_test" \
    "$HELPER_DIR/../app/src/test/resources/call_control/answer_timeout_cancel_v1.txt" \
    "$HELPER_DIR/../app/src/test/resources/call_control/answer_timeout_not_connected_v1.txt"
"$BUILD_DIR/menu_policy_test"
"$BUILD_DIR/mixer_route_policy_test"
"$BUILD_DIR/protocol_test"
"$BUILD_DIR/privacy_policy_test"
"$BUILD_DIR/prompt_barge_in_policy_test"
"$BUILD_DIR/recording_policy_test"
"$BUILD_DIR/sha256_test"
"$BUILD_DIR/revision_config_test" \
    "$HELPER_DIR/../contracts/fixtures/configuration_revision_v3.compiled.txt" \
    "$HELPER_DIR/../contracts/fixtures/configuration_revision_v4.compiled.txt" \
    "$HELPER_DIR/../contracts/fixtures/configuration_revision_v41.compiled.txt"
"$BUILD_DIR/session_snapshot_test"
"$BUILD_DIR/telecom_guard_test"
"$BUILD_DIR/device_profile_test"
