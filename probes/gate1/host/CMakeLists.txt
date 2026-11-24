cmake_minimum_required(VERSION 3.16)

project(deskmate_gate1_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gate1_core
    src/handshake.cpp
    src/protocol.cpp
    src/transfer_set.cpp
)
target_include_directories(gate1_core PUBLIC include)

if(MSVC)
    target_compile_options(gate1_core PRIVATE /W4 /WX)
else()
    target_compile_options(gate1_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

add_executable(gate1_core_tests
    tests/test_main.cpp
)
target_link_libraries(gate1_core_tests PRIVATE gate1_core)
target_include_directories(gate1_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../device/entry/src/main/cpp"
)
target_compile_definitions(gate1_core_tests PRIVATE
    GATE1_MAIN_SOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

if(MSVC)
    target_compile_options(gate1_core_tests PRIVATE /W4 /WX)
else()
    target_compile_options(gate1_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME gate1_core_tests COMMAND gate1_core_tests)

find_path(LIBUSB_INCLUDE_DIR
    NAMES libusb-1.0/libusb.h
    HINTS
        "${CMAKE_PREFIX_PATH}/include"
        /opt/homebrew/include
        /usr/local/include
)
find_library(LIBUSB_LIBRARY
    NAMES usb-1.0 libusb-1.0
    HINTS
        "${CMAKE_PREFIX_PATH}/lib"
        /opt/homebrew/lib
        /usr/local/lib
)

if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    add_executable(gate1_host
        src/main.cpp
        src/aoa.cpp
        src/probe_session.cpp
    )
    target_include_directories(gate1_host PRIVATE include)
    target_include_directories(gate1_host SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(gate1_host PRIVATE gate1_core ${LIBUSB_LIBRARY})
    target_compile_options(gate1_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
    message(STATUS "libusb found: host probe enabled")
else()
    message(WARNING
        "libusb-1.0 was not found. Core tests will build, but `gate1_host` "
        "is skipped. Install libusb, then pass LIBUSB_INCLUDE_DIR and "
        "LIBUSB_LIBRARY to CMake. See probes/gate1/RUNBOOK.md.")
endif()

option(GATE1_LIBUSB_SYNTAX_SHIM
    "Compile the real host sources against a non-runnable libusb API shim"
    OFF
)

option(GATE1_FORCE_LIBUSB_SYNTAX_SHIM
    "Build the syntax shim target even when a real libusb is available"
    OFF
)

if(GATE1_LIBUSB_SYNTAX_SHIM AND
   (GATE1_FORCE_LIBUSB_SYNTAX_SHIM OR NOT (LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)))
    add_executable(gate1_host_syntax
        tools/libusb_shim/libusb_shim.cpp
        src/main.cpp
        src/aoa.cpp
        src/probe_session.cpp
    )
    target_include_directories(gate1_host_syntax PRIVATE
        include
        tools/libusb_shim/include
    )
    target_compile_definitions(gate1_host_syntax PRIVATE
        GATE1_LIBUSB_SYNTAX_SHIM=1
    )
    target_link_libraries(gate1_host_syntax PRIVATE gate1_core)
    target_compile_options(gate1_host_syntax PRIVATE -Wall -Wextra -Wpedantic -Werror)
    message(STATUS "Building non-runnable libusb syntax shim target")
endif()
