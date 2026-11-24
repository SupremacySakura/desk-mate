cmake_minimum_required(VERSION 3.5.0)

project(gate1_probe)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gate1_probe SHARED
    napi_init.cpp
    accessory_probe.cpp
    probe_protocol.cpp
)

target_include_directories(gate1_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gate1_probe PRIVATE -pthread)
target_link_libraries(gate1_probe PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    pthread
)
