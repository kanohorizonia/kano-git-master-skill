// converge command — deterministic sync+push with resumable state

#include <CLI/CLI.hpp>

#include "command_runtime_ops.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

namespace kano::git::commands {
namespace {

auto Trim(std::string InValue) -> std::string {
    while (!InValue.empty() && (InValue.back() == '\n' || InValue.back() == '\r' || InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() && (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto ConvergeStatePath(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    return (InWorkspaceRoot / ".kano" / "tmp" / "converge.state").lexically_normal();
}

auto ReadState(const std::filesystem::path& InStatePath) -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> out;
    std::ifstream in(InStatePath, std::ios::binary);
    if (!in.good()) {
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, eq));
        const auto value = Trim(line.substr(eq + 1));
        if (!key.empty()) {
            out[key] = value;
        }
    }
    return out;
}

auto WriteState(const std::filesystem::path& InStatePath,
                const std::filesystem::path& InWorkspaceRoot,
                const std::string& InPhase,
                const bool InRecursive,
                const bool InDryRun) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(InStatePath.parent_path(), ec);
    std::ofstream out(InStatePath, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    out << "phase=" << InPhase << "\n";
    out << "workspace_root=" << InWorkspaceRoot.lexically_normal().generic_string() << "\n";
    out << "recursive=" << (InRecursive ? "1" : "0") << "\n";
    out << "dry_run=" << (InDryRun ? "1" : "0") << "\n";
    return out.good();
}

auto DeleteState(const std::filesystem::path& InStatePath) -> void {
    std::error_code ec;
    std::filesystem::remove(InStatePath, ec);
}

} // namespace

void RegisterConverge(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("converge", "Converge workspace to synced+pushed state with resumable phases");

    auto* noRecursive = new bool{false};
    auto* dryRun = new bool{false};
    auto* statusOnly = new bool{false};
    auto* resume = new bool{false};
    auto* abort = new bool{false};
    auto* profile = new bool{false};
    auto* verbose = new bool{false};
    auto* forceWithLease = new bool{false};
    auto* noVerify = new bool{false};
    auto* jobs = new int{1};
    auto* remote = new std::string{};

    cmd->add_flag("--no-recursive,-N", *noRecursive, "Only run on current repository");
    cmd->add_flag("--dry-run", *dryRun, "Preview converge actions without changing repositories");
    cmd->add_flag("--status", *statusOnly, "Show current converge state");
    cmd->add_flag("--resume", *resume, "Resume from saved converge phase");
    cmd->add_flag("--abort", *abort, "Abort converge and remove saved state");
    cmd->add_flag("--profile", *profile, "Print push profile summary");
    cmd->add_flag("--verbose", *verbose, "Verbose push output");
    cmd->add_flag("--force-with-lease", *forceWithLease, "Pass --force-with-lease to converge push stage");
    cmd->add_flag("--no-verify", *noVerify, "Pass --no-verify to converge push stage");
    cmd->add_option("--jobs", *jobs, "Parallel workers for converge push stage");
    cmd->add_option("--remote", *remote, "Optional remote filter for converge push stage");

    cmd->callback([=]() {
        const auto workspaceRoot = std::filesystem::current_path().lexically_normal();
        const auto recursive = !*noRecursive;
        const auto statePath = ConvergeStatePath(workspaceRoot);

        const int controlFlags = (*statusOnly ? 1 : 0) + (*resume ? 1 : 0) + (*abort ? 1 : 0);
        if (controlFlags > 1) {
            std::cerr << "Error: --status, --resume, and --abort are mutually exclusive\n";
            std::exit(2);
        }

        if (*jobs < 1) {
            std::cerr << "Error: --jobs must be a positive integer\n";
            std::exit(2);
        }

        if (*statusOnly) {
            const auto state = ReadState(statePath);
            if (state.empty()) {
                std::cout << "converge state: none\n";
                std::exit(0);
            }

            std::cout << "converge state file: " << statePath.generic_string() << "\n";
            for (const auto& [key, value] : state) {
                std::cout << key << "=" << value << "\n";
            }
            std::exit(0);
        }

        if (*abort) {
            DeleteState(statePath);
            std::cout << "converge state removed: " << statePath.generic_string() << "\n";
            std::exit(0);
        }

        std::string phase = "sync";
        if (*resume) {
            const auto state = ReadState(statePath);
            if (state.empty()) {
                std::cerr << "Error: no converge state to resume\n";
                std::exit(1);
            }
            const auto it = state.find("phase");
            if (it != state.end() && !it->second.empty()) {
                phase = it->second;
            }
            std::cout << "Resuming converge from phase: " << phase << "\n";
        }

        if (!WriteState(statePath, workspaceRoot, phase, recursive, *dryRun)) {
            std::cerr << "Error: failed to write converge state file: " << statePath.generic_string() << "\n";
            std::exit(1);
        }

        if (phase == "sync") {
            std::cout << "[converge] phase=sync\n";
            const auto syncCode = RunSyncOriginLatestNative(workspaceRoot, recursive, *dryRun, false);
            if (syncCode != 0) {
                WriteState(statePath, workspaceRoot, "sync", recursive, *dryRun);
                std::exit(syncCode);
            }
            phase = "push";
            if (!WriteState(statePath, workspaceRoot, phase, recursive, *dryRun)) {
                std::cerr << "Error: failed to write converge state transition to push\n";
                std::exit(1);
            }
        }

        if (phase == "push") {
            std::cout << "[converge] phase=push\n";
            const auto pushCode = RunPushNativeSimple(
                workspaceRoot,
                recursive,
                *dryRun,
                *profile,
                *forceWithLease,
                *noVerify,
                *jobs,
                *verbose,
                *remote);
            if (pushCode != 0) {
                WriteState(statePath, workspaceRoot, "push", recursive, *dryRun);
                std::exit(pushCode);
            }
            phase = "done";
        }

        if (!WriteState(statePath, workspaceRoot, phase, recursive, *dryRun)) {
            std::cerr << "Error: failed to finalize converge state\n";
            std::exit(1);
        }

        DeleteState(statePath);
        std::cout << "[converge] completed\n";
        std::exit(0);
    });
}

} // namespace kano::git::commands
