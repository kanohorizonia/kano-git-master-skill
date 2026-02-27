// clone command — Smart clone with upstream support
// Delegates to: scripts/core/smart-clone.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

namespace kano::git::commands {

void RegisterClone(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("clone", "Smart clone with upstream remote support");

    auto* repoUrl = new std::string{};
    auto* upstreamUrl = new std::string{};
    auto* targetDir = new std::string{};
    auto* noInit = new bool{false};
    auto* noCheckout = new bool{false};
    auto* dryRun = new bool{false};

    cmd->add_option("repo-url", *repoUrl, "Repository URL to clone")->required();
    cmd->add_option("upstream-url", *upstreamUrl, "Optional upstream repository URL");
    cmd->add_option("--dir", *targetDir, "Target directory");
    cmd->add_flag("--no-init", *noInit, "Skip initializing if remote is empty");
    cmd->add_flag("--no-checkout", *noCheckout, "Skip checkout to default branch");
    cmd->add_flag("--dry-run", *dryRun, "Show what would be done");

    cmd->callback([=]() {
        std::vector<std::string> args;
        args.push_back(*repoUrl);
        if (!upstreamUrl->empty()) {
            args.push_back(*upstreamUrl);
        }
        if (!targetDir->empty()) {
            args.push_back("--dir");
            args.push_back(*targetDir);
        }
        if (*noInit) {
            args.push_back("--no-init");
        }
        if (*noCheckout) {
            args.push_back("--no-checkout");
        }
        if (*dryRun) {
            args.push_back("--dry-run");
        }

        auto extras = cmd->remaining();
        args.insert(args.end(), extras.begin(), extras.end());

        auto result = shell::ExecuteScript("core/smart-clone.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
