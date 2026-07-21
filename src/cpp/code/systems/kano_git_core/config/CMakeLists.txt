add_library(kano_git_config STATIC)

target_sources(kano_git_config
  PRIVATE
    private/kog_config.cpp
  PUBLIC
    public/kog_config.hpp
)

kog_configure_kano_module(kano_git_config)

target_include_directories(kano_git_config
  PUBLIC
    public
  PRIVATE
    private
)

target_link_libraries(kano_git_config
  PUBLIC
    kano::git_shell
  PRIVATE
    tomlplusplus::tomlplusplus
)

add_library(kano::git_config ALIAS kano_git_config)
