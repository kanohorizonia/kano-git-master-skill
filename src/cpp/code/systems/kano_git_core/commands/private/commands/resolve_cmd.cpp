// resolve command — AI-powered conflict resolution
// Delegates to: scripts/commit-tools/resolve/smart-resolve.sh (and variants)

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {

void RegisterResolve(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("resolve", "AI-powered conflict resolution");
    cmd->allow_extras();

    auto provider = std::make_shared<std::string>();
    cmd->add_option("--provider,-p", *provider, "AI provider (copilot, codex, opencode)")
        ->default_str("auto");

    cmd->callback([cmd, provider]() {
        auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native-only mode for resolve.";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            throw CLI::RuntimeError(2);
        }

        if (!provider->empty() && *provider != "auto") {
            std::cout << "[resolve] --provider is a compatibility flag in native resolve mode\n";
        }

        const auto inRepo = shell::ExecuteCommand(
            "git",
            {"rev-parse", "--is-inside-work-tree"},
            shell::ExecMode::Capture
        );
        if (inRepo.exitCode != 0 || inRepo.stdoutStr.find("true") == std::string::npos) {
            std::cerr << "Error: not inside a git repository\n";
            throw CLI::RuntimeError(1);
        }

        const auto conflicts = shell::ExecuteCommand(
            "git",
            {"diff", "--name-only", "--diff-filter=U"},
            shell::ExecMode::Capture
        );
        if (conflicts.exitCode != 0) {
            std::cerr << "Error: failed to inspect merge conflicts\n";
            throw CLI::RuntimeError(conflicts.exitCode);
        }

        std::vector<std::string> files;
        std::istringstream iss(conflicts.stdoutStr);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                files.push_back(line);
            }
        }

        if (files.empty()) {
            std::cout << "No unresolved merge conflicts found.\n";
            std::exit(0);
        }

        std::cerr << "Native resolve found unresolved conflicts:\n";
        for (const auto& file : files) {
            std::cerr << "  - " << file << "\n";
        }
        std::cerr << "\nNext steps:\n";
        std::cerr << "  1) Resolve files manually or with your IDE\n";
        std::cerr << "  2) Stage resolved files: git add <file>\n";
        std::cerr << "  3) Continue merge/rebase workflow\n";
        std::exit(1);
    });
}

} // namespace kano::git::commands
