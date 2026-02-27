// push command — Multi-remote push workflow
// Delegates to: scripts/commit-tools/smart-push.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterPush(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("push", "Multi-remote push workflow");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("commit-tools/smart-push.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
