add_library(kano_git_cmd_repo_sync STATIC)

target_sources(kano_git_cmd_repo_sync
  PRIVATE
    private/converge_cmd.cpp
    private/foreach_cmd.cpp
    private/push_cmd.cpp
    private/reset_cmd.cpp
    private/remote_cmd.cpp
    private/stash_cmd.cpp
    private/submodule_cmd.cpp
    private/subtree_cmd.cpp
    private/sync_cli.cpp
    private/sync_cmd.cpp
    private/update_cmd.cpp
    private/worktree_cmd.cpp
)

kog_configure_kano_module(kano_git_cmd_repo_sync)

target_include_directories(kano_git_cmd_repo_sync
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/repo_sync/private
)

target_link_libraries(kano_git_cmd_repo_sync PUBLIC kano::git_shell kano::git_workspace kano::git_command_runtime nlohmann_json::nlohmann_json)

add_library(kano::git_cmd_repo_sync ALIAS kano_git_cmd_repo_sync)
