add_library(kano_git_core STATIC)

target_sources(kano_git_core
  PRIVATE
    version/public/version.hpp
    shell/public/shell_executor.hpp
    commands/public/command_registry.hpp
    commands/public/metadata_cache.hpp
    commands/public/autocomplete_engine.hpp
    commands/public/command_mode_state.hpp
    commands/public/command_executor.hpp
    commands/public/tui_state.hpp
    workspace/public/discovery.hpp
    workspace/public/native_workspace.hpp

    shell/private/shell_executor.cpp
    commands/private/command_registry.cpp
    commands/private/metadata_cache.cpp
    commands/private/autocomplete_engine.cpp
    commands/private/command_mode_state.cpp
    commands/private/command_executor.cpp
    commands/private/tui_state.cpp

    workspace/private/discovery.cpp
    workspace/private/scheduler.hpp
    workspace/private/scheduler.cpp
    workspace/private/native_workspace.cpp

    commands/private/commands/branch_cmd.cpp
    commands/private/commands/cache_cmd.cpp
    commands/private/commands/clone_cmd.cpp
    commands/private/commands/complete_cmd.cpp
    commands/private/commands/completion_cmd.cpp
    commands/private/commands/commit_cmd.cpp
    commands/private/commands/commit_push_cmd.cpp
    commands/private/commands/dirty_cmd.cpp
    commands/private/commands/doctor_cmd.cpp
    commands/private/commands/guide_cmd.cpp
    commands/private/commands/log_cmd.cpp
    commands/private/commands/meta_cmd.cpp
    commands/private/commands/p4_cmd.cpp
    commands/private/commands/plan_cmd.cpp
    commands/private/commands/push_cmd.cpp
    commands/private/commands/remote_cmd.cpp
    commands/private/commands/resolve_cmd.cpp
    commands/private/commands/self_cmd.cpp
    commands/private/commands/scalar_cmd.cpp
    commands/private/commands/status_cmd.cpp
    commands/private/commands/submodule_cmd.cpp
    commands/private/commands/subtree_cmd.cpp
    commands/private/commands/svn_cmd.cpp
    commands/private/commands/sync_cmd.cpp
    commands/private/commands/tui_cmd.cpp
    commands/private/commands/version_cmd.cpp
    commands/private/commands/workspace_cmd.cpp
    commands/private/commands/worktree_cmd.cpp
)

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_core
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        version/public/version.cppm
        shell/public/shell.cppm
        commands/public/commands.cppm
        workspace/public/workspace.cppm
  )
  target_compile_definitions(kano_git_core PUBLIC KOG_USE_MODULES=1)
endif()

add_library(kano::git_core ALIAS kano_git_core)

# Set C++23 standard and disable module scanning (MSBuild bug workaround)
target_compile_features(kano_git_core PUBLIC cxx_std_23)
if(KOG_ENABLE_MODULES)
  set_target_properties(kano_git_core PROPERTIES CXX_SCAN_FOR_MODULES ON)
else()
  set_target_properties(kano_git_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()
# CLI11 v2.4 uses <codecvt> which is removed in MSVC 19.50 (VS 2026)
target_compile_definitions(kano_git_core PRIVATE CLI11_HAS_CODECVT=0)
target_include_directories(kano_git_core 
    PUBLIC
      version/public
      shell/public
      commands/public
      workspace/public
    PRIVATE
      shell/private
      commands/private
      commands/private/commands
      workspace/private
)

# Workaround for MSBuild failing to find standard headers due to -external:W0 on SYSTEM includes
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    if(DEFINED cli11_SOURCE_DIR AND EXISTS "${cli11_SOURCE_DIR}/include")
        target_compile_options(kano_git_core PRIVATE "/I${cli11_SOURCE_DIR}/include")
    endif()
endif()

target_link_libraries(kano_git_core PUBLIC CLI11::CLI11 ftxui::screen ftxui::dom ftxui::component)

# Add tests subdirectory
option(KOG_BUILD_TESTS "Build TUI command input enhancement tests" ON)
if(KOG_BUILD_TESTS)
    add_subdirectory(tests)
endif()
