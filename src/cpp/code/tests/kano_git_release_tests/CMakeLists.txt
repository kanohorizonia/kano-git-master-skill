add_executable(kano_git_release_tests
  release_metadata_test.cpp
)

target_link_libraries(kano_git_release_tests
  PRIVATE kano_git_test_main
  PRIVATE kano::git_cmd_product
)

target_include_directories(kano_git_release_tests
  PRIVATE "${KANO_GIT_COMMAND_ROOT}/product/private"
)

target_compile_definitions(kano_git_release_tests
  PRIVATE KANO_GIT_TEST_REPO_ROOT="${KOG_REPO_ROOT}"
  PRIVATE KANO_GIT_TEST_CLI_BINARY="$<TARGET_FILE:kano_git_cli>"
)

add_dependencies(kano_git_release_tests kano_git_cli)

catch_discover_tests(kano_git_release_tests PROPERTIES ${KOG_TEST_PROPERTIES})
kano_infra_finalize_local_test_timeouts(300)
