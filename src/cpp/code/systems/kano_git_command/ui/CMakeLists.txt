add_library(kano_git_cmd_ui STATIC)

target_sources(kano_git_cmd_ui
  PRIVATE
    private/metadata_cache.cpp
    private/autocomplete_engine.cpp
    private/command_mode_state.cpp
    private/tui_state.cpp
    private/tui_cmd.cpp
    private/tui_dashboard_runner.cpp
  PUBLIC
    public/metadata_cache.hpp
    public/autocomplete_engine.hpp
    public/command_mode_state.hpp
    public/tui_state.hpp
    public/tui_dashboard_runner.hpp
)

kog_configure_kano_module(kano_git_cmd_ui)

target_include_directories(kano_git_cmd_ui
  PUBLIC
    ${KANO_GIT_COMMAND_ROOT}/ui/public
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/ui/private
)

target_link_libraries(kano_git_cmd_ui PUBLIC CLI11::CLI11 ftxui::screen ftxui::dom ftxui::component kano::git_shell kano::git_workspace kano::git_command_runtime)

add_library(kano::git_cmd_ui ALIAS kano_git_cmd_ui)
