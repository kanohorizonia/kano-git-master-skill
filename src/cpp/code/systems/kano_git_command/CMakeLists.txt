set(KANO_GIT_COMMAND_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(runtime)
add_subdirectory(repo_sync)
add_subdirectory(commit_plan)
add_subdirectory(ui)
add_subdirectory(bridge)
add_subdirectory(product)

add_library(kano_git_command INTERFACE)
add_library(kano::git_command ALIAS kano_git_command)

# GCC uses a single-pass linker that processes libraries left-to-right.
# The command sub-libraries have circular dependencies (runtime calls Register*()
# from product/repo_sync/etc., which depend back on runtime). MSVC rescans
# automatically; GCC needs explicit --start-group/--end-group to resolve cycles.
target_link_libraries(kano_git_command
  INTERFACE
    "$<$<CXX_COMPILER_ID:GNU>:-Wl,--start-group>"
    kano::git_command_runtime
    kano::git_cmd_repo_sync
    kano::git_cmd_commit_plan
    kano::git_cmd_ui
    kano::git_cmd_bridge
    kano::git_cmd_product
    "$<$<CXX_COMPILER_ID:GNU>:-Wl,--end-group>"
)
