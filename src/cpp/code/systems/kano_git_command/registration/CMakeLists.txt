add_library(kano_git_command_registration STATIC)

target_sources(kano_git_command_registration
  PRIVATE
    private/command_declarations.hpp
    private/command_registry.cpp
  PUBLIC
    public/command_registry.hpp
)

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_command_registration
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        public/commands.cppm
  )
endif()

kog_configure_kano_module(kano_git_command_registration)

target_include_directories(kano_git_command_registration
  PUBLIC
    public
  PRIVATE
    private
)

target_link_libraries(kano_git_command_registration
  PUBLIC
    CLI11::CLI11
    kano::git_command_runtime
    kano::git_cmd_repo_sync
    kano::git_cmd_commit_plan
    kano::git_cmd_ui
    kano::git_cmd_bridge
    kano::git_cmd_product
)

add_library(kano::git_command_registration ALIAS kano_git_command_registration)
