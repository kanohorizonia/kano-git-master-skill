// Command registry — wires up all subcommands

#include "command_registry.hpp"
#include "command_declarations.hpp"

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
    RegisterConfig(InApp);
    RegisterDirty(InApp);
    RegisterDiscover(InApp);
    RegisterDoctor(InApp);
    RegisterForeach(InApp);
    RegisterGuide(InApp);
    RegisterIgnore(InApp);
    RegisterLog(InApp);
    RegisterMeta(InApp);
    RegisterP4(InApp);
    RegisterPlan(InApp);
    RegisterPush(InApp);
    RegisterRepo(InApp);
    RegisterRemote(InApp);
    RegisterReset(InApp);
    RegisterResolve(InApp);
    RegisterSelf(InApp);
    RegisterScalar(InApp);
    RegisterSlog(InApp);
    RegisterStatus(InApp);
    RegisterSubmodule(InApp);
    RegisterSubtree(InApp);
    RegisterSvn(InApp);
    RegisterSync(InApp);
    RegisterTui(InApp);
    RegisterUpdate(InApp);
    RegisterUplog(InApp);
    RegisterVersion(InApp);
    RegisterWorktree(InApp);
}

} // namespace kano::git::commands
