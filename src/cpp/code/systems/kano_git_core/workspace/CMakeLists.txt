add_library(kano_git_workspace STATIC)

target_sources(kano_git_workspace
  PRIVATE
    public/discovery.hpp
    public/native_workspace.hpp
    private/discovery.cpp
    private/scheduler.hpp
    private/scheduler.cpp
    private/native_workspace.cpp
)

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_workspace
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        public/workspace.cppm
  )
endif()

kog_configure_kano_module(kano_git_workspace)

target_include_directories(kano_git_workspace
  PUBLIC
    public
  PRIVATE
    private
)

target_link_libraries(kano_git_workspace PUBLIC kano::git_shell kano::git_command_runtime)

add_library(kano::git_workspace ALIAS kano_git_workspace)
