// doctor command — Environment and repo health checks
// Delegates to: scripts/commit-tools/doctor.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <iostream>
#include <string>

namespace kano::git::commands {

void RegisterDoctor(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("doctor", "Environment and repository health checks");
    cmd->allow_extras();

    cmd->callback([=]() {
        auto extras = cmd->remaining();
        if (!extras.empty()) {
            std::cerr << "Error: unsupported extra arguments in native-only mode for doctor.";
            for (const auto& extra : extras) {
                std::cerr << ' ' << extra;
            }
            std::cerr << "\n";
            std::exit(2);
        }

        int failures = 0;
        std::cout << "=== Native Doctor ===\n";

        const auto gitVersion = shell::ExecuteCommand("git", {"--version"}, shell::ExecMode::Capture);
        if (gitVersion.exitCode != 0) {
            std::cerr << "[FAIL] git is not available in PATH\n";
            std::exit(1);
        }
        std::cout << "[PASS] " << gitVersion.stdoutStr;

        const auto inRepo = shell::ExecuteCommand(
            "git",
            {"rev-parse", "--is-inside-work-tree"},
            shell::ExecMode::Capture
        );
        if (inRepo.exitCode != 0 || inRepo.stdoutStr.find("true") == std::string::npos) {
            std::cerr << "[FAIL] not inside a git repository\n";
            std::exit(1);
        }
        std::cout << "[PASS] inside git repository\n";

        const auto branch = shell::ExecuteCommand(
            "git",
            {"rev-parse", "--abbrev-ref", "HEAD"},
            shell::ExecMode::Capture
        );
        if (branch.exitCode == 0) {
            std::cout << "[INFO] current branch: " << branch.stdoutStr;
        }

        const auto status = shell::ExecuteCommand(
            "git",
            {"status", "--porcelain"},
            shell::ExecMode::Capture
        );
        if (status.exitCode == 0) {
            if (status.stdoutStr.empty()) {
                std::cout << "[PASS] working tree is clean\n";
            } else {
                std::cout << "[WARN] working tree has local changes\n";
            }
        } else {
            std::cerr << "[FAIL] unable to read git status\n";
            failures += 1;
        }

        const auto origin = shell::ExecuteCommand(
            "git",
            {"remote", "get-url", "origin"},
            shell::ExecMode::Capture
        );
        if (origin.exitCode == 0) {
            std::cout << "[PASS] origin remote configured\n";
        } else {
            std::cout << "[WARN] origin remote not configured\n";
        }

        const auto upstream = shell::ExecuteCommand(
            "git",
            {"remote", "get-url", "upstream"},
            shell::ExecMode::Capture
        );
        if (upstream.exitCode == 0) {
            std::cout << "[PASS] upstream remote configured\n";
        } else {
            std::cout << "[INFO] upstream remote not configured\n";
        }

        const auto scalar = shell::ExecuteCommand("git", {"scalar", "--help"}, shell::ExecMode::Capture);
        if (scalar.exitCode == 0) {
            std::cout << "[PASS] git scalar available\n";
        } else {
            std::cout << "[INFO] git scalar not available\n";
        }

        const auto bun = shell::ExecuteCommand("bun", {"--version"}, shell::ExecMode::Capture);
        if (bun.exitCode == 0) {
            std::cout << "[PASS] bun available: " << bun.stdoutStr;
        } else {
            std::cout << "[INFO] bun not available in PATH\n";
        }

        std::cout << "=== Doctor complete ===\n";
        std::exit(failures == 0 ? 0 : 1);
    });
}

} // namespace kano::git::commands
