add_library(kano_git_shell STATIC)

target_sources(kano_git_shell
  PRIVATE
    public/shell_executor.hpp
    private/shell_executor.cpp
)

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_shell
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        public/shell.cppm
  )
endif()

kog_configure_kano_module(kano_git_shell)

target_include_directories(kano_git_shell
  PUBLIC
    public
  PRIVATE
    private
)

add_library(kano::git_shell ALIAS kano_git_shell)
