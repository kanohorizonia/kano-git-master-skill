// clone command — Smart clone with upstream support
// Delegates to: scripts/core/smart-clone.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <memory>

namespace kano::git::commands {

void RegisterClone(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("clone", "Smart clone with upstream remote support");

    struct CloneOptions {
        std::string repoUrl;
        std::string upstreamUrl;
        std::string targetDir;
        bool noInit = false;
        bool noCheckout = false;
        bool dryRun = false;
    };

    auto options = std::make_shared<CloneOptions>();

    cmd->add_option("repo-url", options->repoUrl, "Repository URL to clone")->required();
    cmd->add_option("upstream-url", options->upstreamUrl, "Optional upstream repository URL");
    cmd->add_option("--dir", options->targetDir, "Target directory");
    cmd->add_flag("--no-init", options->noInit, "Skip initializing if remote is empty");
    cmd->add_flag("--no-checkout", options->noCheckout, "Skip checkout to default branch");
    cmd->add_flag("--dry-run", options->dryRun, "Show what would be done");

    cmd->callback([cmd, options]() {
        std::vector<std::string> args;
        args.push_back(options->repoUrl);
        if (!options->upstreamUrl.empty()) {
            args.push_back(options->upstreamUrl);
        }
        if (!options->targetDir.empty()) {
            args.push_back("--dir");
            args.push_back(options->targetDir);
        }
        if (options->noInit) {
            args.push_back("--no-init");
        }
        if (options->noCheckout) {
            args.push_back("--no-checkout");
        }
        if (options->dryRun) {
            args.push_back("--dry-run");
        }

        auto extras = cmd->remaining();
        args.insert(args.end(), extras.begin(), extras.end());

        auto result = shell::ExecuteScript("core/smart-clone.sh", args);
        if (result.exitCode != 0) {
            throw CLI::RuntimeError(result.exitCode);
        }
    });
}

} // namespace kano::git::commands
