add_library(kano_git_cmd_bridge STATIC)

target_sources(kano_git_cmd_bridge
  PRIVATE
    private/p4_cmd.cpp
    private/scalar_cmd.cpp
    private/svn_cmd.cpp
)

kog_configure_kano_module(kano_git_cmd_bridge)

target_include_directories(kano_git_cmd_bridge
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/bridge/private
)

target_link_libraries(kano_git_cmd_bridge PUBLIC kano::git_shell kano::git_workspace kano::git_command_runtime)

add_library(kano::git_cmd_bridge ALIAS kano_git_cmd_bridge)
