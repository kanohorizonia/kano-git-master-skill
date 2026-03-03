// Command registry — wires up all subcommands

#include "command_registry.hpp"

namespace kano::git::commands {

void RegisterAll(CLI::App& InApp) {
    // Top-level commands sorted A-Z for predictable help output
    RegisterAmend(InApp);
    RegisterBranch(InApp);
    RegisterCache(InApp);
    RegisterClone(InApp);
    RegisterCommit(InApp);
    RegisterCommitPush(InApp);
    RegisterComplete(InApp);
    RegisterCompletion(InApp);
    RegisterDirty(InApp);
    RegisterDoctor(InApp);
    RegisterGuide(InApp);
    RegisterLog(InApp);
    RegisterMeta(InApp);
    RegisterP4(InApp);
    RegisterPush(InApp);
    RegisterRemote(InApp);
    RegisterResolve(InApp);
    RegisterScalar(InApp);
    RegisterSlog(InApp);
    RegisterStatus(InApp);
    RegisterSubmodule(InApp);
    RegisterSubtree(InApp);
    RegisterSvn(InApp);
    RegisterSync(InApp);
    RegisterTui(InApp);
    RegisterUplog(InApp);
    RegisterVersion(InApp);
    RegisterWorkspace(InApp);
    RegisterWorktree(InApp);
}

} // namespace kano::git::commands
