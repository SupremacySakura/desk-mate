add_executable(deskmate-windows-contract-tests contract_tests.cpp)
target_sources(deskmate-windows-contract-tests PRIVATE
    ../src/capture_encoder.cpp
    ../src/input_injector.cpp
    ../src/updater.cpp
)
target_link_libraries(deskmate-windows-contract-tests PRIVATE
    deskmate_windows_link
)
add_test(NAME deskmate-windows-contract-tests COMMAND deskmate-windows-contract-tests)
