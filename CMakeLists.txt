cmake_minimum_required(VERSION 3.20)

project(DeskMate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(deskmate_protocol STATIC
    core/src/lifecycle.cpp
    core/src/message_stream.cpp
    core/src/protocol.cpp
    core/src/recovery.cpp
    core/src/scheduler.cpp
    core/src/session.cpp
)

target_include_directories(deskmate_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

add_subdirectory(apps/common)

option(DESKMATE_BUILD_WINDOWS_HOST "Build the Windows host" OFF)
option(DESKMATE_BUILD_ANDROID_CLIENT "Build the Android client native library" OFF)
option(DESKMATE_BUILD_PLATFORM_TESTS "Build cross-platform contract tests" ON)

# The link/session library also builds on macOS and Linux so the AOA pump,
# multi-device routing and 12 KiB fragment policy stay testable without a
# Windows toolchain. Only the executable (DXGI/MF/SendInput) is Windows-only.
if(DESKMATE_BUILD_WINDOWS_HOST)
    add_subdirectory(apps/windows-host)
endif()

if(DESKMATE_BUILD_ANDROID_CLIENT AND ANDROID)
    add_subdirectory(apps/android-client/app/src/main/cpp)
endif()

if(DESKMATE_BUILD_PLATFORM_TESTS AND NOT ANDROID)
    add_subdirectory(apps/common/tests)
endif()
