# Test infrastructure for TUI Command Input Enhancement
cmake_minimum_required(VERSION 3.21)

# Fetch Catch2 for testing
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# Enable testing
enable_testing()

# Test data generators library
add_library(tui_test_generators STATIC
    generators/tui_state_generator.cpp
    generators/command_string_generator.cpp
)
target_link_libraries(tui_test_generators
    PUBLIC Catch2::Catch2
)
target_include_directories(tui_test_generators
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Unit tests
add_executable(tui_unit_tests
    unit/test_main.cpp
    unit/test_metadata_cache.cpp
    unit/test_autocomplete_engine.cpp
    unit/test_command_mode_state.cpp
    unit/test_command_executor.cpp
    unit/test_tui_state.cpp
    unit/test_shell_executor.cpp
)
target_link_libraries(tui_unit_tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE tui_test_generators
    PRIVATE kano_git_core
)

# Property-based tests
add_executable(tui_property_tests
    property/test_main.cpp
    property/test_autocomplete_properties.cpp
    property/test_command_mode_state_properties.cpp
    property/test_command_executor_properties.cpp
    property/test_tui_state_properties.cpp
)
target_link_libraries(tui_property_tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE tui_test_generators
    PRIVATE kano_git_core
)

# Integration tests
add_executable(tui_integration_tests
    integration/test_main.cpp
)
target_link_libraries(tui_integration_tests
    PRIVATE Catch2::Catch2WithMain
    PRIVATE tui_test_generators
)

# Register tests with CTest
include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tui_unit_tests)
catch_discover_tests(tui_property_tests)
catch_discover_tests(tui_integration_tests)

# Add custom target to run all TUI tests
add_custom_target(run_tui_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS tui_unit_tests tui_property_tests tui_integration_tests
    COMMENT "Running all TUI command input tests"
)
