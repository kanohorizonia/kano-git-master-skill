# Test infrastructure for kano-git
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

# Register tests with CTest
include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../systems/kano_git_test_core
                 ${CMAKE_BINARY_DIR}/code/systems/kano_git_test_core)
add_subdirectory(kano_git_cli_tests)
add_subdirectory(kano_git_tui_tests)
add_subdirectory(e2e)

if(WIN32)
  add_custom_target(run_kano_git_tests
    COMMAND pwsh -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.ps1" -Preset ${KOG_PRESET_NAME}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running fast kano-git test lane"
  )

  add_custom_target(run_kano_git_all_tests
    COMMAND pwsh -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.ps1" -Preset ${KOG_PRESET_NAME} -WithE2E
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running full kano-git test lane (CLI/TUI/E2E)"
  )
else()
  add_custom_target(run_kano_git_tests
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" ${KOG_PRESET_NAME}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running fast kano-git test lane"
  )

  add_custom_target(run_kano_git_all_tests
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" ${KOG_PRESET_NAME} --with-e2e
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running full kano-git test lane (CLI/TUI/E2E)"
  )
endif()
