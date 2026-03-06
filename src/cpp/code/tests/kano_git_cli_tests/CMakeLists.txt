add_executable(kano_git_cli_tests
  functional/functional_test_main.cpp
)

target_link_libraries(kano_git_cli_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano_git_test_core
)

target_include_directories(kano_git_cli_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

catch_discover_tests(kano_git_cli_tests)
