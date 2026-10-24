# Test infrastructure for kano-git
cmake_minimum_required(VERSION 3.21)

# Fetch Catch2 for testing
include(FetchContent)
kog_fetchcontent_declare_git(
    Catch2
    REPOSITORY https://github.com/catchorg/Catch2.git
    TAG v3.5.2
)
# KOG-BUG-0107 r18: Catch2's own CMakeLists publishes a global
# BUILD_SHARED_LIBS=ON cache value, so Catch2 was being built as
# libCatch2.dll on windows-ninja-msvc.  The test executable then
# depends on that DLL, and the catch_discover_tests discovery run
# (which executes kano_git_tui_tests.exe --list-tests before CTest can
# run anything) dies with STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139),
# aborting the whole TUI PR Gates build.
#
# Build Catch2 statically so the test executables are self-contained.
# This mirrors the existing BUILD_SHARED_LIBS save/restore dance that
# kano_git_commit_plan_tests already uses for rapidcheck (which has the
# same "no Windows DLL ABI" constraint and the same root cause: a
# fetched dependency publishing a global BUILD_SHARED_LIBS cache value).
if(DEFINED BUILD_SHARED_LIBS)
  set(_kog_catch2_build_shared_libs_was_defined TRUE)
  set(_kog_catch2_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
else()
  set(_kog_catch2_build_shared_libs_was_defined FALSE)
endif()
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(Catch2)
if(_kog_catch2_build_shared_libs_was_defined)
  set(BUILD_SHARED_LIBS "${_kog_catch2_saved_build_shared_libs}")
else()
  unset(BUILD_SHARED_LIBS)
endif()
unset(_kog_catch2_build_shared_libs_was_defined)
unset(_kog_catch2_saved_build_shared_libs)

# Enable testing
enable_testing()

# Register tests with CTest
include(CTest)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# Compile startup in each test executable's CRT, including discovery runs.
add_library(kano_git_test_main INTERFACE)
target_sources(kano_git_test_main INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common/test_main.cpp")
target_link_libraries(kano_git_test_main INTERFACE Catch2::Catch2 KanoInfra::unattended)
kano_infra_test_properties(KOG_TEST_PROPERTIES 300)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../systems/kano_git_test_core
                 ${CMAKE_BINARY_DIR}/code/systems/kano_git_test_core)
add_subdirectory(kano_git_cli_tests)
add_subdirectory(kano_git_export_tests)
add_subdirectory(kano_git_release_tests)
add_subdirectory(kano_git_regression_tests)
add_subdirectory(kano_git_tui_tests)
add_subdirectory(kano_git_commit_plan_tests)
add_subdirectory(kano_git_integration_tests)
add_subdirectory(e2e)

if(WIN32)
  add_custom_target(run_kano_git_tests
    COMMAND pwsh -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.ps1" -Preset ${KOG_PRESET_NAME}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running fast kano-git test lane"
  )

  add_custom_target(run_kano_git_all_tests
    COMMAND pwsh -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.ps1" -Preset ${KOG_PRESET_NAME} -WithIntegration -WithE2E
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running full kano-git test lane (CLI/TUI/integration/E2E)"
  )
else()
  add_custom_target(run_kano_git_tests
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" ${KOG_PRESET_NAME}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running fast kano-git test lane"
  )

  add_custom_target(run_kano_git_all_tests
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh" ${KOG_PRESET_NAME} full --with-integration --with-e2e
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running full kano-git test lane (CLI/TUI/integration/E2E)"
  )
endif()

add_custom_target(run_kano_git_integration_tests
  COMMAND $<TARGET_FILE:kano_infra_tool> watchdog --timeout-ms 1800000 --cleanup-timeout-ms 5000 -- $<TARGET_FILE:kano_git_integration_tests>
  DEPENDS kano_git_integration_tests kano_infra_tool
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Running native kano-git integration test lane"
  VERBATIM
)
