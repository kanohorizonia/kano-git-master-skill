// Command registry — wires up all subcommands

#include "command_registry.hpp"
#include "command_declarations.hpp"

namespace kano::git::commands {

void RegisterAll(CLI::App& InApp) {
    // Top-level commands sorted A-Z for predictable help output
    RegisterAmend(InApp);
    RegisterBranch(InApp);
    RegisterCache(InApp);
    RegisterCherryPick(InApp);
    RegisterClone(InApp);
    RegisterCommit(InApp);
    RegisterCommitPush(InApp);
    RegisterComplete(InApp);
    RegisterCompletion(InApp);
    RegisterConfig(InApp);
    RegisterDiscover(InApp);
    RegisterDoctor(InApp);
    RegisterExport(InApp);
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
    RegisterRepoHygiene(InApp);
    RegisterReset(InApp);
    RegisterResolve(InApp);
    RegisterSelf(InApp);
    RegisterScalar(InApp);
    RegisterSlog(InApp);
    RegisterStash(InApp);
    RegisterStatus(InApp);
    RegisterSubmodule(InApp);
    RegisterSubtree(InApp);
    RegisterSvn(InApp);
    RegisterSync(InApp);
    RegisterTui(InApp);
    RegisterUpdate(InApp);
    RegisterUplog(InApp);
    RegisterVersion(InApp);
    RegisterWorkspace(InApp);
    RegisterWorktree(InApp);

    auto setGroup = [&](const std::string& name, const std::string& groupName) {
        try {
            if (auto* sub = InApp.get_subcommand(name)) {
                sub->group(groupName);
            }
        } catch (...) {
            // ignore
        }
    };

    // 1. Basic / Common Operations
    const std::string commonGroup = "1. Basic / Common Operations";
    setGroup("amend", commonGroup);
    setGroup("commit", commonGroup);
    setGroup("commit-push", commonGroup);
    setGroup("push", commonGroup);
    setGroup("sync", commonGroup);
    setGroup("status", commonGroup);
    setGroup("log", commonGroup);
    setGroup("slog", commonGroup);
    setGroup("reset", commonGroup);
    setGroup("update", commonGroup);
    setGroup("tui", commonGroup);

    // 2. Repository Management
    const std::string repoGroup = "2. Repository Management";
    setGroup("clone", repoGroup);
    setGroup("branch", repoGroup);
    setGroup("remote", repoGroup);
    setGroup("worktree", repoGroup);
    setGroup("submodule", repoGroup);
    setGroup("subtree", repoGroup);
    setGroup("workspace", repoGroup);
    setGroup("stash", repoGroup);
    setGroup("repo-hygiene", repoGroup);

    // 3. AI & Planning
    const std::string aiGroup = "3. AI & Planning";
    setGroup("plan", aiGroup);
    setGroup("resolve", aiGroup);
    setGroup("doctor", aiGroup);
    setGroup("ignore", aiGroup);

    // 4. Advanced Operations
    const std::string advancedGroup = "4. Advanced Operations";
    setGroup("cherry-pick", advancedGroup);
    setGroup("uplog", advancedGroup);
    setGroup("foreach", advancedGroup);
    setGroup("scalar", advancedGroup);
    setGroup("p4", advancedGroup);
    setGroup("svn", advancedGroup);

    // 5. Maintenance & System
    const std::string maintenanceGroup = "5. Maintenance & System";
    setGroup("self", maintenanceGroup);
    setGroup("cache", maintenanceGroup);
    setGroup("config", maintenanceGroup);
    setGroup("export", maintenanceGroup);
    setGroup("meta", maintenanceGroup);

    // Hidden commands
    setGroup("complete", "");
    setGroup("completion", "");
    setGroup("discover", "");
    setGroup("repo", "");
    setGroup("version", "");
    setGroup("guide", "");
}

} // namespace kano::git::commands
