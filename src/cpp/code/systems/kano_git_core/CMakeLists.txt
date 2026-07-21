add_subdirectory(shell)
add_subdirectory(build_info)
add_subdirectory(config)
add_subdirectory(workspace)

add_library(kano_git_core INTERFACE)
add_library(kano::git_core ALIAS kano_git_core)

target_link_libraries(kano_git_core
  INTERFACE
    kano::git_shell
    kano::git_workspace
    kano::git_build_info
)

# Workaround for MSBuild failing to find standard headers due to -external:W0 on SYSTEM includes
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    if(DEFINED cli11_SOURCE_DIR AND EXISTS "${cli11_SOURCE_DIR}/include")
        target_compile_options(kano_git_shell PRIVATE "/I${cli11_SOURCE_DIR}/include")
        target_compile_options(kano_git_workspace PRIVATE "/I${cli11_SOURCE_DIR}/include")
    endif()
endif()
