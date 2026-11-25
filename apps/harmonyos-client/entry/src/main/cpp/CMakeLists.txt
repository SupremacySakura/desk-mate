cmake_minimum_required(VERSION 3.5.0)

project(deskmate_device)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DESKMATE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
set(DESKMATE_CORE "${DESKMATE_ROOT}/core")

add_library(deskmate_device SHARED
    napi_init.cpp
    device_engine.cpp
    fd_byte_io.cpp
    video_decoder.cpp
    ${DESKMATE_CORE}/src/lifecycle.cpp
    ${DESKMATE_CORE}/src/message_stream.cpp
    ${DESKMATE_CORE}/src/protocol.cpp
    ${DESKMATE_CORE}/src/recovery.cpp
    ${DESKMATE_CORE}/src/scheduler.cpp
    ${DESKMATE_CORE}/src/session.cpp
)

target_include_directories(deskmate_device PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DESKMATE_CORE}/include
)

target_compile_options(deskmate_device PRIVATE -pthread -fexceptions)

target_link_libraries(deskmate_device PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
    libnative_media_vdec.so
    libnative_media_core.so
    libnative_media_codecbase.so
    libnative_window.so
    pthread
)
