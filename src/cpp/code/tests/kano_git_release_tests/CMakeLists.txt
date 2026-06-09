add_executable(kano_git_release_tests
  release_metadata_test.cpp
)

target_link_libraries(kano_git_release_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano::git_cmd_product
)

target_include_directories(kano_git_release_tests
  PRIVATE "${KANO_GIT_COMMAND_ROOT}/product/private"
)

target_compile_definitions(kano_git_release_tests
  PRIVATE KANO_GIT_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../../../../.."
)

catch_discover_tests(kano_git_release_tests)
