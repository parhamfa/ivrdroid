cmake_minimum_required(VERSION 3.22)

project(ivrdroid_helper LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -g0
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.
)
add_link_options(
    -Wl,--build-id=sha1
)

add_library(tinyalsa STATIC
    third_party/tinyalsa/src/limits.c
    third_party/tinyalsa/src/mixer.c
    third_party/tinyalsa/src/mixer_hw.c
    third_party/tinyalsa/src/mixer_plugin.c
    third_party/tinyalsa/src/pcm.c
    third_party/tinyalsa/src/pcm_hw.c
    third_party/tinyalsa/src/pcm_plugin.c
    third_party/tinyalsa/src/snd_card_plugin.c
)

target_include_directories(tinyalsa PUBLIC
    third_party/tinyalsa/include
)
target_compile_definitions(tinyalsa PRIVATE
    _POSIX_C_SOURCE=200809L
    TINYALSA_USES_PLUGINS
)
target_compile_options(tinyalsa PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_link_libraries(tinyalsa PUBLIC
    dl
)

add_library(ivrdroid_dtmf STATIC
    src/dtmf_detector.cpp
)
target_include_directories(ivrdroid_dtmf PUBLIC
    src
)
target_compile_options(ivrdroid_dtmf PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_library(ivrdroid_core STATIC
    src/audit_policy.cpp
    src/call_control_protocol.cpp
    src/call_safety_policy.cpp
    src/call_lifetime_policy.cpp
    src/continuous_recording.cpp
    src/child_process.cpp
    src/conversation_handoff_protocol.cpp
    src/device_profile.cpp
    src/external_call_policy.cpp
    src/helper_protocol.cpp
    src/menu_policy.cpp
    src/mixer_route_policy.cpp
    src/privacy_policy.cpp
    src/prompt_barge_in_policy.cpp
    src/recording_policy.cpp
    src/revision_config.cpp
    src/session_snapshot.cpp
    src/sha256.cpp
    src/telecom_guard.cpp
)
target_include_directories(ivrdroid_core PUBLIC
    src
)
target_compile_options(ivrdroid_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(ivrdroid-helper
    src/main.cpp
    src/session_audio.cpp
)

set(IVRDROID_SOURCE_COMMIT "unknown" CACHE STRING "Release source commit")
if(NOT IVRDROID_SOURCE_COMMIT MATCHES "^([0-9a-f]+|unknown)$")
    message(FATAL_ERROR "Invalid source commit")
endif()
target_compile_definitions(ivrdroid-helper PRIVATE IVRDROID_SOURCE_COMMIT="${IVRDROID_SOURCE_COMMIT}")
target_compile_options(ivrdroid-helper PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fstack-protector-strong
)
target_link_libraries(ivrdroid-helper PRIVATE
    android
    ivrdroid_core
    ivrdroid_dtmf
    log
    tinyalsa
)

option(IVRDROID_BUILD_AUDIO_HARNESS "Build an isolated synthetic PCM acceptance harness" OFF)
if(IVRDROID_BUILD_AUDIO_HARNESS)
    add_executable(ivrdroid-audio-harness tests/session_audio_harness.cpp)
    target_include_directories(ivrdroid-audio-harness PRIVATE third_party/tinyalsa/include)
    target_compile_definitions(ivrdroid-audio-harness PRIVATE
        IVRDROID_AUDIT_BRIDGE="/data/local/tmp/ivrdroid-session-audit-tests")
    target_compile_options(ivrdroid-audio-harness PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    target_link_libraries(ivrdroid-audio-harness PRIVATE ivrdroid_core)
endif()
