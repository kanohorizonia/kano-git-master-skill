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

# Set C++20 standard and disable module scanning (MSBuild bug workaround)
target_compile_features(kano_git_core PUBLIC cxx_std_20)
set_target_properties(kano_git_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
# CLI11 v2.4 uses <codecvt> which is removed in MSVC 19.50 (VS 2026)
target_compile_definitions(kano_git_core PRIVATE CLI11_HAS_CODECVT=0)
target_include_directories(kano_git_core PRIVATE 
    src
    ../thirdparty
    src/modules
)

target_link_libraries(kano_git_core PUBLIC CLI11::CLI11)
