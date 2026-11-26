cmake_minimum_required(VERSION 3.20)

project(deskmate_windows_host LANGUAGES CXX)

add_library(deskmate_windows_link STATIC
    src/aoa_links.cpp
    src/display_topology.cpp
    src/host_state.cpp
    src/libusb_aoa.cpp
    src/virtual_display.cpp
)

target_include_directories(deskmate_windows_link PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(deskmate_windows_link PUBLIC
    deskmate_platform_common
)

target_compile_definitions(deskmate_windows_link PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
)
if(MSVC)
    target_compile_options(deskmate_windows_link PRIVATE /utf-8)
endif()

find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
find_library(LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0)

if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    target_include_directories(deskmate_windows_link PUBLIC ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(deskmate_windows_link PUBLIC ${LIBUSB_LIBRARY})
    target_compile_definitions(deskmate_windows_link PUBLIC DESKMATE_HAVE_LIBUSB=1)
else()
    message(WARNING "libusb-1.0 not found; the AOA link will build in stub mode")
endif()

if(WIN32)
    enable_language(RC)
    add_executable(DeskMateHost WIN32
        src/main.cpp
        src/capture_encoder.cpp
        src/host_controller.cpp
        src/input_injector.cpp
        src/studio_ui.cpp
        src/updater.cpp
        resources/DeskMateHost.rc
    )
    target_include_directories(DeskMateHost PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
    )
    target_link_libraries(DeskMateHost PRIVATE
        deskmate_windows_link
        d2d1
        dwrite
        dwmapi
        d3d11
        dxgi
        windowsapp
        d3dcompiler
        dxguid
        mfplat
        mfreadwrite
        mfuuid
        wmcodecdspuuid
        shcore
        shell32
        advapi32
        wtsapi32
        winmm
        avrt
        winhttp
        setupapi
        cfgmgr32
        ole32
        oleaut32
        user32
        gdi32
    )
    target_compile_definitions(DeskMateHost PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        # DXGI Desktop Duplication, Media Foundation, DirectWrite and
        # SetProcessDpiAwarenessContext all need the Windows 10 SDK surface.
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )
    if(MSVC)
        target_compile_options(DeskMateHost PRIVATE /utf-8)
    endif()
endif()

if(DESKMATE_BUILD_PLATFORM_TESTS AND NOT WIN32)
    add_subdirectory(tests)
endif()
