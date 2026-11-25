add_executable(deskmate-platform-tests platform_tests.cpp)

target_link_libraries(deskmate-platform-tests PRIVATE
    deskmate_platform_common
)

add_test(NAME deskmate-platform-tests COMMAND deskmate-platform-tests)

add_executable(deskmate-wire-compat-tests wire_compat_tests.cpp)
target_link_libraries(deskmate-wire-compat-tests PRIVATE
    deskmate_platform_common
)
add_test(NAME deskmate-wire-compat-tests COMMAND deskmate-wire-compat-tests)

add_executable(deskmate-aoa-session-tests aoa_session_tests.cpp)
target_link_libraries(deskmate-aoa-session-tests PRIVATE
    deskmate_platform_common
)
target_include_directories(deskmate-aoa-session-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../windows-host/include
)
target_sources(deskmate-aoa-session-tests PRIVATE
    ../../windows-host/src/aoa_links.cpp
)
add_test(NAME deskmate-aoa-session-tests COMMAND deskmate-aoa-session-tests)

add_executable(deskmate-update-tests update_tests.cpp)
target_link_libraries(deskmate-update-tests PRIVATE
    deskmate_platform_common
)
add_test(NAME deskmate-update-tests COMMAND deskmate-update-tests)
