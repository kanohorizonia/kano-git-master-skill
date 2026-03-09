set(KANO_GIT_COMMAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(runtime)
add_subdirectory(repo_sync)
add_subdirectory(commit_plan)
add_subdirectory(ui)
add_subdirectory(bridge)
add_subdirectory(product)

add_library(kano_git_command INTERFACE)
add_library(kano::git_command ALIAS kano_git_command)

target_link_libraries(kano_git_command
  INTERFACE
    kano::git_command_runtime
    kano::git_cmd_repo_sync
    kano::git_cmd_commit_plan
    kano::git_cmd_ui
    kano::git_cmd_bridge
    kano::git_cmd_product
)
