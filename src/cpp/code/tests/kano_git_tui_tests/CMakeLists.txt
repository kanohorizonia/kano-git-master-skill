add_executable(kano_git_tui_tests
  unit/test_main.cpp
  unit/test_metadata_cache.cpp
  unit/test_autocomplete_engine.cpp
  unit/test_command_mode_state.cpp
  unit/test_command_executor.cpp
  unit/test_tui_state.cpp
  unit/test_shell_executor.cpp
  property/test_main.cpp
  property/test_autocomplete_properties.cpp
  property/test_command_mode_state_properties.cpp
  property/test_command_executor_properties.cpp
  property/test_tui_state_properties.cpp
  integration/test_main.cpp
)

target_link_libraries(kano_git_tui_tests
  PRIVATE Catch2::Catch2WithMain
  PRIVATE kano_git_test_core
  PRIVATE kano_git_core
)

catch_discover_tests(kano_git_tui_tests)
