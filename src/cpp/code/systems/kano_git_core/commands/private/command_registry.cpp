// Command registry — wires up all subcommands

#include "command_registry.hpp"

namespace kano::git::commands {

void RegisterAll(CLI::App& InApp) {
    RegisterVersion(InApp);
    RegisterMeta(InApp);
    RegisterComplete(InApp);
    RegisterCompletion(InApp);

    // Smart tools (AI-powered)
    RegisterCommit(InApp);
    RegisterResolve(InApp);
    RegisterSync(InApp);
    RegisterPush(InApp);

    // Repository structure
    RegisterWorktree(InApp);
    RegisterSubtree(InApp);
    RegisterSubmodule(InApp);
    RegisterScalar(InApp);

    // VCS bridges
    RegisterP4(InApp);
    RegisterSvn(InApp);

    // Branch & workspace operations
    RegisterBranch(InApp);
    RegisterWorkspace(InApp);

    // Utilities
    RegisterClone(InApp);
    RegisterDoctor(InApp);
}

} // namespace kano::git::commands
