cmake_minimum_required(VERSION 3.5.0)
project(deskmate_gate3_probe)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GATE3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
set(GATE3_CORE "${GATE3_ROOT}/core")

add_library(entry SHARED
    napi_init.cpp
    ohos_decoder.cpp
    probe_engine.cpp
    ${GATE3_CORE}/src/annexb.cpp
    ${GATE3_CORE}/src/lifecycle_controller.cpp
    ${GATE3_CORE}/src/probe_cleanup.cpp
    ${GATE3_CORE}/src/recovery_output.cpp
    ${GATE3_CORE}/src/surface_lease.cpp
)

target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GATE3_CORE}/include
)

target_link_libraries(entry PUBLIC
    libnative_media_vdec.so
    libnative_media_core.so
    libnative_media_codecbase.so
    libnative_window.so
    libace_napi.z.so
    libace_ndk.z.so
    libhilog_ndk.z.so
)
