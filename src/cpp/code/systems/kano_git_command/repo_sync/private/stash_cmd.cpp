// stash command — manage git stashes across workspace repos

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "native_workspace.hpp"
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

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

auto GitCapture(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> shell::ExecResult {
    return shell::ExecuteCommand("git", InArgs, shell::ExecMode::Capture, InRepo);
}

void ListStashes(const std::filesystem::path& root, int maxDepth) {
    workspace::DiscoverOptions options;
    options.rootDir = root;
    options.maxDepth = maxDepth;
    const auto discovery = workspace::DiscoverRepos(options);

    bool foundAny = false;
    for (const auto& repo : discovery.repos) {
        auto result = GitCapture(repo.path, {"stash", "list"});
        if (result.exitCode == 0 && !Trim(result.stdoutStr).empty()) {
            foundAny = true;
            std::cout << "[" << kano::terminal::Wrap(repo.path.generic_string(), kano::terminal::Color::BoldCyan) << "]\n";
            std::cout << result.stdoutStr;
            if (result.stdoutStr.back() != '\n') {
                std::cout << "\n";
            }
        }
    }
    if (!foundAny) {
        std::cout << "No stashes found in the workspace.\n";
    }
}

void ClearAllStashes(const std::filesystem::path& root, int maxDepth) {
    workspace::DiscoverOptions options;
    options.rootDir = root;
    options.maxDepth = maxDepth;
    const auto discovery = workspace::DiscoverRepos(options);

    for (const auto& repo : discovery.repos) {
        auto result = GitCapture(repo.path, {"stash", "list"});
        if (result.exitCode == 0 && !Trim(result.stdoutStr).empty()) {
            GitCapture(repo.path, {"stash", "clear"});
            std::cout << "Cleared stashes in " << repo.path.generic_string() << "\n";
        }
    }
}

void ClearOldStashes(const std::filesystem::path& root, int maxDepth, int daysOld) {
    workspace::DiscoverOptions options;
    options.rootDir = root;
    options.maxDepth = maxDepth;
    const auto discovery = workspace::DiscoverRepos(options);

    std::string expireStr = std::to_string(daysOld) + ".days.ago";
    
    for (const auto& repo : discovery.repos) {
        auto result = GitCapture(repo.path, {"stash", "list"});
        if (result.exitCode == 0 && !Trim(result.stdoutStr).empty()) {
            GitCapture(repo.path, {"reflog", "expire", "--expire=" + expireStr, "refs/stash"});
            std::cout << "Pruned stashes older than " << daysOld << " days in " << repo.path.generic_string() << "\n";
        }
    }
}

} // namespace

void RegisterStash(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("stash", "Manage git stashes recursively across the workspace");

    auto* repo = new std::string{"."};
    auto* maxDepth = new int{12};
    cmd->add_option("--repo", *repo, "Target repository root path");
    cmd->add_option("--native-max-depth", *maxDepth, "Native discovery max depth (0 = unlimited)");

    auto* list = cmd->add_subcommand("list", "List all stashes across all submodules");
    list->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        ListStashes(root, *maxDepth);
    });

    auto* clear = cmd->add_subcommand("clear", "Clear stashes older than N days (default: 14)");
    auto* days = new int{14};
    clear->add_option("--days", *days, "Clear stashes older than this many days");
    clear->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        ClearOldStashes(root, *maxDepth, *days);
    });

    auto* clearall = cmd->add_subcommand("clearall", "Force clear all stashes in all repos");
    clearall->callback([=]() {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::path(*repo));
        ClearAllStashes(root, *maxDepth);
    });
}

} // namespace kano::git::commands
