# Fetch rapidcheck for property-based testing.
# RC_ENABLE_CATCH is intentionally left OFF to avoid rapidcheck pulling in its
# own bundled Catch2 (ext/catch), which would conflict with the project's
# Catch2 FetchContent target.  We add the extras/catch INTERFACE target
# manually after FetchContent_MakeAvailable.
include(FetchContent)
FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG ff6af6fc683159deb51c543b065eba14dfcf329b
)
set(RC_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RC_ENABLE_CATCH    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidcheck)

# Suppress MSVC /WX (warnings-as-errors) for rapidcheck, which triggers
# C4392 warnings in system headers when built with the project's global flags.
if(MSVC AND TARGET rapidcheck)
    target_compile_options(rapidcheck PRIVATE /WX-)
endif()

# Manually create the rapidcheck_catch INTERFACE target (mirrors
# extras/catch/CMakeLists.txt) so we can use <rapidcheck/catch.h> without
# pulling in rapidcheck's bundled Catch2.
if(NOT TARGET rapidcheck_catch)
    add_library(rapidcheck_catch INTERFACE)
    target_link_libraries(rapidcheck_catch INTERFACE rapidcheck)
    target_include_directories(rapidcheck_catch INTERFACE
        "${rapidcheck_SOURCE_DIR}/extras/catch/include"
    )
endif()

add_executable(kano_git_export_tests
  export_helpers_property_test.cpp
  export_unit_test.cpp
  export_cli_registration_test.cpp
  export_integration_test.cpp
)

target_link_libraries(kano_git_export_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano_git_test_core
  PRIVATE kano::git_cmd_product
  PRIVATE rapidcheck
  PRIVATE rapidcheck_catch
)

target_include_directories(kano_git_export_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE "${KANO_GIT_COMMAND_ROOT}/product/private"
)

catch_discover_tests(kano_git_export_tests)
