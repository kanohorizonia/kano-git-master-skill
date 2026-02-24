add_library(kano_git_core STATIC)

target_sources(kano_git_core
  PRIVATE
    src/modules/KanoGit.ShellExecutor.hpp
    src/modules/KanoGit.CommandRegistry.hpp
    src/modules/KanoGit.Version.hpp
    src/shell_executor.cpp
    src/command_registry.cpp
    src/commands/BranchCmd.cpp
    src/commands/CloneCmd.cpp
    src/commands/CommitCmd.cpp
    src/commands/DoctorCmd.cpp
    src/commands/P4Cmd.cpp
    src/commands/PushCmd.cpp
    src/commands/ResolveCmd.cpp
    src/commands/ScalarCmd.cpp
    src/commands/SubmoduleCmd.cpp
    src/commands/SubtreeCmd.cpp
    src/commands/SvnCmd.cpp
    src/commands/SyncCmd.cpp
    src/commands/VersionCmd.cpp
    src/commands/WorkspaceCmd.cpp
    src/commands/WorktreeCmd.cpp
)

add_library(kano::git_core ALIAS kano_git_core)

# Public includes
target_compile_features(kano_git_core PUBLIC cxx_std_23)
target_include_directories(kano_git_core PRIVATE 
    src
    ../thirdparty/fmt-10.2.1/include
    ../thirdparty
    src/modules
)

target_compile_definitions(kano_git_core PRIVATE FMT_HEADER_ONLY=1)
target_link_libraries(kano_git_core PUBLIC CLI11::CLI11 fmt::fmt)
