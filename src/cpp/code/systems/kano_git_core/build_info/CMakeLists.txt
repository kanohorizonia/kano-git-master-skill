add_library(kano_git_build_info STATIC)

target_sources(kano_git_build_info
  PRIVATE
    public/build_info.hpp
)

if(NOT KOG_ENABLE_MODULES)
  target_sources(kano_git_build_info
    PRIVATE
      private/build_info.cpp
  )
endif()

if(KOG_ENABLE_MODULES)
  target_sources(kano_git_build_info
    PUBLIC
      FILE_SET cxx_modules TYPE CXX_MODULES FILES
        public/build_info.cppm
  )
endif()

kog_configure_kano_module(kano_git_build_info)

target_compile_definitions(kano_git_build_info
  PUBLIC
    KOG_BUILD_VERSION="${KOG_BUILD_VERSION_STR}"
    KOG_BUILD_VCS="${KOG_BUILD_VCS_STR}"
    KOG_BUILD_BRANCH="${KOG_BUILD_BRANCH_STR}"
    KOG_BUILD_REVISION="${KOG_BUILD_REVISION_STR}"
    KOG_BUILD_REVISION_HASH_SHORT="${KOG_BUILD_REVISION_HASH_SHORT_STR}"
    KOG_BUILD_REVISION_HASH="${KOG_BUILD_REVISION_HASH_STR}"
    KOG_BUILD_DIRTY="${KOG_BUILD_DIRTY_STR}"
    KOG_BUILD_HOST_NAME="${KOG_BUILD_HOST_NAME_STR}"
    KOG_BUILD_CI="${KOG_BUILD_CI_STR}"
    KOG_BUILD_CONTEXT="${KOG_BUILD_CONTEXT_STR}"
    KOG_BUILD_PIPELINE_ID="${KOG_BUILD_PIPELINE_ID_STR}"
    KOG_BUILD_PLATFORM="${KOG_BUILD_PLATFORM_STR}"
    KOG_BUILD_TOOLCHAIN="${KOG_BUILD_TOOLCHAIN_STR}"
    KOG_BUILD_GENERATOR="${KOG_BUILD_GENERATOR_STR}"
    KOG_BUILD_PRESET="${KOG_BUILD_PRESET_STR}"
    KOG_BUILD_CONFIGURATION="${KOG_BUILD_CONFIGURATION_STR}"
)

target_include_directories(kano_git_build_info
  PUBLIC
    public
)

add_library(kano::git_build_info ALIAS kano_git_build_info)
