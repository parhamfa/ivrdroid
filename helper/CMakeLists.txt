cmake_minimum_required(VERSION 3.22)

project(ivrdroid_helper LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(ivrdroid-helper
    src/main.cpp
)

target_compile_options(ivrdroid-helper PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fstack-protector-strong
)
target_link_libraries(ivrdroid-helper PRIVATE
    android
    ivrdroid_dtmf
    log
    tinyalsa
)
