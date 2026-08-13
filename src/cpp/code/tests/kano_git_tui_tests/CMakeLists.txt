add_executable(kano_git_tui_tests
  unit/test_main.cpp
  unit/test_metadata_cache.cpp
  unit/test_autocomplete_engine.cpp
  unit/test_command_mode_state.cpp
  unit/test_command_executor.cpp
  unit/test_tui_state.cpp
  unit/test_shell_executor.cpp
  unit/test_tui_async_lifecycle.cpp
  unit/test_tui_audit_frame.cpp
  unit/test_tui_audit_surface.cpp
  unit/test_tui_command_scope.cpp
  unit/test_tui_history_lifecycle.cpp
  unit/test_tui_history_patch.cpp
  unit/test_tui_keymap.cpp
  unit/test_tui_load_state.cpp
  unit/test_tui_rebase_model.cpp
  unit/test_tui_repo_identity.cpp
  unit/test_tui_startup_snapshot.cpp
  unit/test_tui_terminal_guard.cpp
  unit/test_tui_theme.cpp
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
  PRIVATE kano_git_command
)

# The production-path TUI integration tests execute both built TUI entrypoints.
# Keep this dependency explicit so a clean focused test build never relies on
# an executable left behind by another target or preset.
add_dependencies(kano_git_tui_tests kano_git_cli kano_git_tui)

target_include_directories(kano_git_tui_tests
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/ui/private
)

catch_discover_tests(kano_git_tui_tests)
