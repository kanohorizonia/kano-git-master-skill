add_executable(kano_git_integration_tests
  integration_test_support.cpp
  process_executor_cases.cpp
  git_transport_cases.cpp
  commit_push_cases.cpp
)

target_link_libraries(kano_git_integration_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano_git_test_core
  PRIVATE kano_git_core
)

target_include_directories(kano_git_integration_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

catch_discover_tests(
  kano_git_integration_tests
  TEST_PREFIX "integration::"
  PROPERTIES LABELS "integration"
)
