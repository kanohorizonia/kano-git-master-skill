add_library(kano_git_cmd_product STATIC)

target_sources(kano_git_cmd_product
  PRIVATE
    private/branch_cmd.cpp
    private/cache_cmd.cpp
    private/clone_cmd.cpp
    private/complete_cmd.cpp
    private/completion_cmd.cpp
    private/config_cmd.cpp
    private/export_cmd.cpp
    private/guide_cmd.cpp
    private/log_cmd.cpp
    private/meta_cmd.cpp
    private/self_cmd.cpp
    private/status_cmd.cpp
    private/version_cmd.cpp
    private/discover_cmd.cpp
    private/repo_hygiene_cmd.cpp
)

kog_configure_kano_module(kano_git_cmd_product)

target_include_directories(kano_git_cmd_product
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/product/private
)

target_link_libraries(kano_git_cmd_product PUBLIC kano::git_shell kano::git_workspace kano::git_command_runtime kano::git_build_info)

add_library(kano::git_cmd_product ALIAS kano_git_cmd_product)
