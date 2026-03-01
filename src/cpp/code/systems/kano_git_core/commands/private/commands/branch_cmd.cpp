// branch command — Branch operations (native C++)

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

auto Trim(std::string value) -> std::string {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        start += 1;
    }
    return value.substr(start);
}

auto GitCapture(const std::vector<std::string>& args) -> shell::ExecResult {
    return shell::ExecuteCommand("git", args, shell::ExecMode::Capture);
}

auto GitPassThrough(const std::vector<std::string>& args) -> shell::ExecResult {
    return shell::ExecuteCommand("git", args, shell::ExecMode::PassThrough);
}

auto ResolveDefaultRemoteBranch(const std::string& preferredRemote) -> std::string {
    std::vector<std::string> remotes;
    if (!preferredRemote.empty()) {
        remotes.push_back(preferredRemote);
    }
    if (preferredRemote != "upstream") {
        remotes.push_back("upstream");
    }
    if (preferredRemote != "origin") {
        remotes.push_back("origin");
    }

    for (const auto& remote : remotes) {
        const auto head = GitCapture({"symbolic-ref", "--quiet", "--short", std::string("refs/remotes/") + remote + "/HEAD"});
        if (head.exitCode == 0) {
            const auto ref = Trim(head.stdoutStr);
            if (!ref.empty()) {
                return ref;
            }
        }

        const auto show = GitCapture({"remote", "show", remote});
        if (show.exitCode == 0) {
            std::istringstream iss(show.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                auto pos = line.find("HEAD branch:");
                if (pos != std::string::npos) {
                    auto branch = Trim(line.substr(pos + std::string("HEAD branch:").size()));
                    if (!branch.empty() && branch != "(unknown)") {
                        return remote + "/" + branch;
                    }
                }
            }
        }
    }
    return {};
}

auto CurrentBranch() -> std::string {
    const auto out = GitCapture({"rev-parse", "--abbrev-ref", "HEAD"});
    if (out.exitCode != 0) {
        return {};
    }
    const auto branch = Trim(out.stdoutStr);
    if (branch == "HEAD") {
        return {};
    }
    return branch;
}

auto ParseBatchFile(const std::string& filePath) -> std::vector<std::string> {
    std::ifstream in(filePath);
    std::vector<std::string> commits;
    if (!in.is_open()) {
        return commits;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.rfind("#", 0) == 0) {
            continue;
        }
        commits.push_back(trimmed);
    }
    return commits;
}

} // namespace

void RegisterBranch(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("branch", "Branch operations");

    auto* rebase = cmd->add_subcommand("rebase-upstream", "Rebase to upstream latest");
    auto* rebaseRemote = new std::string{"upstream"};
    auto* rebaseOnto = new std::string{};
    rebase->add_option("--remote", *rebaseRemote, "Remote used to resolve default branch (upstream|origin)")->default_str("upstream");
    rebase->add_option("--onto", *rebaseOnto, "Explicit upstream target (e.g., upstream/main)");
    rebase->allow_extras();
    rebase->callback([=]() {
        auto target = Trim(*rebaseOnto);
        if (target.empty()) {
            target = ResolveDefaultRemoteBranch(Trim(*rebaseRemote));
        }
        if (target.empty()) {
            std::cerr << "Error: cannot resolve upstream default branch. Use --onto <remote/branch>.\n";
            std::exit(1);
        }

        auto slash = target.find('/');
        if (slash == std::string::npos || slash == 0) {
            std::cerr << "Error: invalid --onto target: " << target << " (expected remote/branch).\n";
            std::exit(1);
        }
        const auto remote = target.substr(0, slash);

        if (const auto fetch = GitPassThrough({"fetch", remote}); fetch.exitCode != 0) {
            std::exit(fetch.exitCode);
        }

        if (const auto rb = GitPassThrough({"rebase", target}); rb.exitCode != 0) {
            std::exit(rb.exitCode);
        }

        std::cout << "Rebased onto " << target << "\n";
        std::exit(0);
    });

    auto* compare = cmd->add_subcommand("compare", "Compare two branches");
    auto* branchA = new std::string{};
    auto* branchB = new std::string{};
    compare->add_option("--a", *branchA, "Left branch/ref (default: HEAD)");
    compare->add_option("--b", *branchB, "Right branch/ref (default: upstream default)");
    compare->allow_extras();
    compare->callback([=]() {
        auto left = Trim(*branchA);
        auto right = Trim(*branchB);

        if (left.empty()) {
            left = "HEAD";
        }
        if (right.empty()) {
            right = ResolveDefaultRemoteBranch("upstream");
            if (right.empty()) {
                right = ResolveDefaultRemoteBranch("origin");
            }
        }

        if (right.empty()) {
            std::cerr << "Error: cannot resolve compare target. Use --b <branch>.\n";
            std::exit(1);
        }

        const auto counts = GitCapture({"rev-list", "--left-right", "--count", left + "..." + right});
        if (counts.exitCode != 0) {
            std::exit(counts.exitCode);
        }

        std::istringstream iss(Trim(counts.stdoutStr));
        int ahead = 0;
        int behind = 0;
        iss >> ahead >> behind;

        std::cout << "Compare " << left << " vs " << right << "\n";
        std::cout << "  ahead:  " << ahead << "\n";
        std::cout << "  behind: " << behind << "\n";

        const auto log = GitPassThrough({"log", "--oneline", "--decorate", "--graph", "--max-count", "20", left + "..." + right});
        std::exit(log.exitCode);
    });

    auto* cherry = cmd->add_subcommand("cherry-pick-batch", "Batch cherry-pick from file");
    auto* batchFile = new std::string{};
    cherry->add_option("--file,-f", *batchFile, "Path to batch file (one commit hash per line)");
    cherry->allow_extras();
    cherry->callback([=]() {
        auto extras = cherry->remaining();
        std::string filePath = Trim(*batchFile);
        if (filePath.empty() && !extras.empty()) {
            filePath = extras.front();
        }

        if (filePath.empty()) {
            std::cerr << "Error: --file is required for cherry-pick-batch\n";
            std::exit(1);
        }

        const auto commits = ParseBatchFile(filePath);
        if (commits.empty()) {
            std::cerr << "Error: no commits found in batch file: " << filePath << "\n";
            std::exit(1);
        }

        for (const auto& commit : commits) {
            std::cout << "Cherry-pick: " << commit << "\n";
            const auto cp = GitPassThrough({"cherry-pick", commit});
            if (cp.exitCode != 0) {
                std::cerr << "Cherry-pick failed at " << commit << ". Resolve conflict then continue manually.\n";
                std::exit(cp.exitCode);
            }
        }

        std::cout << "Cherry-pick batch completed: " << commits.size() << " commit(s)\n";
        std::exit(0);
    });
}

} // namespace kano::git::commands
