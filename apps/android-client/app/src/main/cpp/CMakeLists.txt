cmake_minimum_required(VERSION 3.20)

project(deskmate_android_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DESKMATE_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core" CACHE PATH
    "DeskMate shared core")
set(DESKMATE_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../common" CACHE PATH
    "DeskMate platform common")

# Gradle drives this file directly, but it can also be included from the root
# CMakeLists. Reuse the targets when they already exist instead of redefining
# them.
if(NOT TARGET deskmate_protocol)
    add_library(deskmate_protocol STATIC
        ${DESKMATE_CORE_DIR}/src/lifecycle.cpp
        ${DESKMATE_CORE_DIR}/src/message_stream.cpp
        ${DESKMATE_CORE_DIR}/src/protocol.cpp
        ${DESKMATE_CORE_DIR}/src/recovery.cpp
        ${DESKMATE_CORE_DIR}/src/scheduler.cpp
        ${DESKMATE_CORE_DIR}/src/session.cpp
    )
    target_include_directories(deskmate_protocol PUBLIC
        ${DESKMATE_CORE_DIR}/include)
endif()

if(NOT TARGET deskmate_platform_common)
    add_library(deskmate_platform_common STATIC
        ${DESKMATE_COMMON_DIR}/src/annex_b.cpp
        ${DESKMATE_COMMON_DIR}/src/aoa.cpp
        ${DESKMATE_COMMON_DIR}/src/input_frames.cpp
        ${DESKMATE_COMMON_DIR}/src/session_profile.cpp
    )
    target_include_directories(deskmate_platform_common PUBLIC
        ${DESKMATE_COMMON_DIR}/include)
    target_link_libraries(deskmate_platform_common PUBLIC deskmate_protocol)
endif()

add_library(deskmate_client SHARED
    jni_bridge.cpp
    accessory_io.cpp
    client_engine.cpp
    video_decoder.cpp
)

target_include_directories(deskmate_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(deskmate_client PRIVATE
    deskmate_platform_common
    android
    log
    mediandk
    EGL
    GLESv2
)

target_compile_options(deskmate_client PRIVATE
    -Wall -Wextra -Wpedantic -fexceptions
    $<$<NOT:$<CONFIG:Debug>>:-O3>
)
