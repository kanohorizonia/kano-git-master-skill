// resolve command — AI-powered conflict resolution
// Delegates to: scripts/commit-tools/resolve/smart-resolve.sh (and variants)

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <memory>

namespace kano::git::commands {

void RegisterResolve(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("resolve", "AI-powered conflict resolution");
    cmd->allow_extras();

    auto provider = std::make_shared<std::string>();
    cmd->add_option("--provider,-p", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    cmd->callback([cmd, provider]() {
        std::string script = "commit-tools/resolve/smart-resolve.sh";

        if (!provider->empty() && *provider != "auto") {
            if (*provider == "copilot") script = "commit-tools/resolve/smart-resolve-copilot.sh";
            else if (*provider == "codex") script = "commit-tools/resolve/smart-resolve-codex.sh";
            else if (*provider == "opencode") script = "commit-tools/resolve/smart-resolve-opencode.sh";
        }

        auto extras = cmd->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());

        auto result = shell::ExecuteScript(script, args);
        if (result.exitCode != 0) {
            throw CLI::RuntimeError(result.exitCode);
        }
    });
}

} // namespace kano::git::commands
