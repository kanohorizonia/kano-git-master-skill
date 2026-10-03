add_executable(kano_git_regression_tests
  regression_coverage_test.cpp
)

target_link_libraries(kano_git_regression_tests
  PRIVATE kano_git_test_main
  PRIVATE kano::git_cmd_product
  PRIVATE nlohmann_json::nlohmann_json
)

target_include_directories(kano_git_regression_tests
  PRIVATE "${KANO_GIT_COMMAND_ROOT}/product/private"
)

target_compile_definitions(kano_git_regression_tests
  PRIVATE KANO_GIT_TEST_REPO_ROOT="${KOG_REPO_ROOT}"
)

catch_discover_tests(kano_git_regression_tests PROPERTIES ${KOG_TEST_PROPERTIES})
kano_infra_finalize_local_test_timeouts(300)
