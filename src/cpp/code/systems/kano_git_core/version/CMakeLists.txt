add_library(kano_git_version STATIC)

target_sources(kano_git_version
  PRIVATE
    public/version.hpp
)

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_version
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        public/version.cppm
  )
endif()

kog_configure_kano_module(kano_git_version)

if(NOT KOG_ENABLE_MODULES)
  set_target_properties(kano_git_version PROPERTIES LINKER_LANGUAGE CXX)
endif()

target_include_directories(kano_git_version
  PUBLIC
    public
)

add_library(kano::git_version ALIAS kano_git_version)
