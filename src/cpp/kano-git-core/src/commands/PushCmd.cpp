// push command — Smart multi-remote push
// Delegates to: scripts/commit-tools/smart-push.sh

#include "KanoGit.CommandRegistry.hpp"
#include "KanoGit.ShellExecutor.hpp"

namespace kano::git::commands {

void Registerpush(CLI::App& app) {
    auto* cmd = app.add_subcommand("push", "Smart multi-remote push with fallback");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto& extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/smart-push.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
