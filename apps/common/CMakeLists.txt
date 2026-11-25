cmake_minimum_required(VERSION 3.20)

project(deskmate_platform_common LANGUAGES CXX)

add_library(deskmate_platform_common STATIC
    src/annex_b.cpp
    src/aoa.cpp
    src/input_frames.cpp
    src/session_profile.cpp
)

target_compile_features(deskmate_platform_common PUBLIC cxx_std_20)
target_include_directories(deskmate_platform_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(deskmate_platform_common PUBLIC deskmate_protocol)

if(MSVC)
    target_compile_options(deskmate_platform_common PRIVATE /W4)
else()
    target_compile_options(deskmate_platform_common PRIVATE
        -Wall -Wextra -Wpedantic)
endif()
