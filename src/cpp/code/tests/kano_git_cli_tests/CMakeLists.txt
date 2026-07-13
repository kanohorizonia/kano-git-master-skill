add_executable(kano_git_cli_tests
  functional/functional_test_main.cpp
  functional/bdd_ai_provider_bootstrap_test.cpp
  functional/bdd_scenario_recorder_test.cpp
  functional/agent_queue_exact_commit_test.cpp
  functional/converge_planner_test.cpp
  functional/unknown_command_suggestions_test.cpp
  functional/version_command_surface_test.cpp
  functional/discover_inventory_test.cpp
  functional/fetch_command_test.cpp
  functional/log_remote_preview_test.cpp
  functional/log_invocation_bug_condition_test.cpp
  functional/log_invocation_preservation_test.cpp
  functional/recursive_push_fixture_test.cpp
  functional/recursive_sync_fixture_test.cpp
  functional/repo_health_preflight_test.cpp
  functional/repo_operation_scheduler_test.cpp
  functional/status_recursive_test.cpp
)

target_link_libraries(kano_git_cli_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano_git_test_core
  PRIVATE kano_git_core
)

target_include_directories(kano_git_cli_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

catch_discover_tests(kano_git_cli_tests)
