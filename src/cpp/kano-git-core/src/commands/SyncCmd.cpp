// sync command — Repository synchronization workflows
// Delegates to: scripts/commit-tools/sync/smart-sync*.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registersync(CLI::App& app) {
    auto* cmd = app.add_subcommand("sync", "Repository synchronization workflows");

    // --- sync origin-latest ---
    auto* origin_latest = cmd->add_subcommand("origin-latest", "Sync to origin default branch latest");
    origin_latest->allow_extras();
    origin_latest->callback([=]() {
        auto& extras = origin_latest->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-origin-latest.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync upstream-force-push ---
    auto* upstream_fp = cmd->add_subcommand("upstream-force-push", "Sync from upstream, force-push to origin");
    upstream_fp->allow_extras();
    upstream_fp->callback([=]() {
        auto& extras = upstream_fp->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-upstream-force-push.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync stable-dev ---
    auto* stable_dev = cmd->add_subcommand("stable-dev", "Stable-dev sync (tag-based cherry-pick migration)");
    stable_dev->allow_extras();
    stable_dev->callback([=]() {
        auto& extras = stable_dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-stable-dev.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync dev ---
    auto* dev = cmd->add_subcommand("dev", "Dev sync (upstream default branch tip)");
    dev->allow_extras();
    dev->callback([=]() {
        auto& extras = dev->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/sync/smart-sync-dev.sh", args);
        std::exit(result.exitCode);
    });

    // --- sync (default: auto-detect) ---
    cmd->allow_extras();
    cmd->callback([=]() {
        if (cmd->get_subcommands().empty()) {
            auto& extras = cmd->remaining();
            std::vector<std::string> args(extras.begin(), extras.end());
            auto result = shell::ExecuteScript("commit-tools/sync/smart-sync.sh", args);
            std::exit(result.exitCode);
        }
    });
}

} // namespace kano::git::commands
