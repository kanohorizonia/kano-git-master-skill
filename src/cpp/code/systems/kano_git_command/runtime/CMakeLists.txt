add_library(kano_git_command_runtime STATIC)

target_sources(kano_git_command_runtime
  PRIVATE
    private/auto_model_policy.cpp
    private/command_executor.cpp
    private/ai_utils.cpp
    private/repo_hygiene_cmd.cpp
  PUBLIC
    public/auto_model_policy.hpp
    public/command_runtime_ops.hpp
    public/command_executor.hpp
    public/ai_utils.hpp
    public/terminal_color.hpp
)

kog_configure_kano_module(kano_git_command_runtime)

target_include_directories(kano_git_command_runtime
  PUBLIC
    ${KANO_GIT_COMMAND_ROOT}/runtime/public
  PRIVATE
    ${KANO_GIT_COMMAND_ROOT}/runtime/private
)

target_link_libraries(kano_git_command_runtime
  PUBLIC
    CLI11::CLI11
    kano::git_shell
    kano::git_config
    kano::git_workspace
)
target_link_libraries(kano_git_command_runtime PRIVATE tomlplusplus::tomlplusplus)

add_library(kano::git_command_runtime ALIAS kano_git_command_runtime)
