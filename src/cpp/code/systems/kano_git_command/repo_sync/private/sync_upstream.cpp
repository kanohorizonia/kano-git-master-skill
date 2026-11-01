#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "sync_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kano::git::commands {
namespace {
auto TrimUpstream(std::string value) -> std::string {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
auto GitUpstream(const std::filesystem::path& repo, std::vector<std::string> args) -> shell::ExecResult {
    // Read-only plans must not refresh the index as a side effect of status.
    args.insert(args.begin(), "--no-optional-locks");
    const auto result = shell::ExecuteCommand("git", args, shell::ExecMode::Capture, repo,
        shell::ProgressCallback{}, std::nullopt, shell::CaptureLimits{8 * 1024 * 1024, 64 * 1024});
    if (result.stdoutTruncated || result.stderrTruncated)
        throw std::runtime_error("Git output was truncated; complete upstream preservation could not be verified");
    if (result.stderrStr.find("Process timeout") != std::string::npos ||
        result.stderrStr.find("Process timed out") != std::string::npos)
        throw std::runtime_error("Git command timed out; upstream preservation could not be verified");
    return result;
}
auto LinesUpstream(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> values;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        line = TrimUpstream(line);
        if (!line.empty()) values.push_back(std::move(line));
    }
    return values;
}

auto PathsUpstream(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> paths;
    std::istringstream input(text);
    for (std::string path; std::getline(input, path, '\0');)
        if (!path.empty()) paths.push_back(std::move(path));
    return paths;
}
auto PathsOverlapUpstream(const std::string& left, const std::string& right) -> bool {
#if defined(_WIN32)
    auto a = std::filesystem::u8path(left).lexically_normal().generic_wstring();
    auto b = std::filesystem::u8path(right).lexically_normal().generic_wstring();
    while (!a.empty() && a.back() == L'/') a.pop_back();
    while (!b.empty() && b.back() == L'/') b.pop_back();
    const auto prefix = [](const std::wstring& x, const std::wstring& y) {
        return x.size() <= y.size() &&
            CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(x.size()), TRUE) == CSTR_EQUAL &&
            (x.size() == y.size() || y[x.size()] == L'/');
    };
#else
    auto a = std::filesystem::path(left).lexically_normal().generic_string();
    auto b = std::filesystem::path(right).lexically_normal().generic_string();
    while (!a.empty() && a.back() == '/') a.pop_back();
    while (!b.empty() && b.back() == '/') b.pop_back();
    const auto prefix = [](const std::string& x, const std::string& y) {
        return x.size() <= y.size() && y.compare(0, x.size(), x) == 0 &&
            (x.size() == y.size() || y[x.size()] == '/');
    };
#endif
    return prefix(a, b) || prefix(b, a);
}

auto RunUpstreamSync(const SyncCommandOptions::Upstream& options) -> int {
    const auto fail = [](const std::string& reason) {
        std::cerr << "ERROR: upstream sync: " << reason << '\n';
        return 1;
    };
    const auto repo = std::filesystem::weakly_canonical(options.repo);
    const auto root = GitUpstream(repo, {"rev-parse", "--show-toplevel"});
    if (root.exitCode != 0 || !std::filesystem::equivalent(TrimUpstream(root.stdoutStr), repo))
        return fail("--repo must select an existing repository root");
    const auto source = GitUpstream(repo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (source.exitCode != 0) return fail("select a source branch; detached HEAD is not supported");
    const auto sourceBranch = TrimUpstream(source.stdoutStr);
    const auto status = GitUpstream(repo, {"status", "--porcelain=v1", "--untracked-files=normal"});
    if (status.exitCode != 0 || !TrimUpstream(status.stdoutStr).empty())
        return fail("source worktree/index must be clean; no stash or cleanup was attempted");
    for (const auto* state : {"CHERRY_PICK_HEAD", "MERGE_HEAD", "rebase-merge", "rebase-apply", "sequencer"}) {
        const auto path = GitUpstream(repo, {"rev-parse", "--git-path", state});
        if (path.exitCode != 0) return fail("cannot inspect in-progress Git operations");
        auto candidate = std::filesystem::path(TrimUpstream(path.stdoutStr));
        if (candidate.is_relative()) candidate = repo / candidate;
        if (std::filesystem::exists(candidate)) return fail("finish or abort the existing Git operation first");
    }
    if (options.branch.starts_with("-") || options.branch.starts_with("refs/") ||
        GitUpstream(repo, {"check-ref-format", "--branch", options.branch}).exitCode != 0)
        return fail("invalid fresh branch name");
    const auto existing = GitUpstream(repo, {"show-ref", "--verify", "--quiet", "refs/heads/" + options.branch});
    if (existing.exitCode != 1) return fail("target branch exists or could not be checked; it will not be overwritten");

    const auto resolve = [&](const std::string& ref) -> std::string {
        if (ref.empty() || ref.starts_with("-")) return {};
        const auto result = GitUpstream(repo, {"-c", "core.warnAmbiguousRefs=true", "rev-parse", "--verify",
                                              "--end-of-options", ref + "^{commit}"});
        if (result.exitCode != 0 || !TrimUpstream(result.stderrStr).empty()) return {};
        const auto sha = TrimUpstream(result.stdoutStr);
        return std::regex_match(sha, std::regex("[0-9a-f]{40}")) ? sha : std::string{};
    };
    const auto head = resolve("HEAD");
    const auto base = resolve(options.base);
    const auto target = resolve(options.ref);
    if (head.empty() || base.empty() || target.empty())
        return fail("base/ref must resolve unambiguously to existing commits; fetch first");
    if (GitUpstream(repo, {"merge-base", "--is-ancestor", base, head}).exitCode != 0 ||
        GitUpstream(repo, {"merge-base", "--is-ancestor", base, target}).exitCode != 0)
        return fail("base must be an ancestor of both source HEAD and target");
    const auto range = base + ".." + head;
    const auto merges = GitUpstream(repo, {"rev-list", "--merges", range});
    const auto commits = GitUpstream(repo, {"rev-list", "--reverse", "--topo-order", range});
    if (merges.exitCode != 0 || commits.exitCode != 0 || !TrimUpstream(merges.stdoutStr).empty())
        return fail("the maintained patch range must be linear and contain no merge commits");
    const auto all = LinesUpstream(commits.stdoutStr);
    auto retained = all;
    if (!options.patchesFile.empty()) {
        std::ifstream input(options.patchesFile, std::ios::binary);
        if (!input) return fail("cannot read patches file");
        retained.clear();
        std::set<std::string> seen;
        std::size_t next = 0;
        for (std::string line; std::getline(input, line);) {
            line = TrimUpstream(line);
            if (line.empty() || line.starts_with("#")) continue;
            if (!std::regex_match(line, std::regex("[0-9a-fA-F]{40}")))
                return fail("patches file requires one full 40-character commit SHA per line");
            std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!seen.insert(line).second) return fail("patches file contains a duplicate commit");
            const auto found = std::find(all.begin() + static_cast<std::ptrdiff_t>(next), all.end(), line);
            if (found == all.end()) return fail("patches must be an ancestry-ordered subset of base..HEAD");
            next = static_cast<std::size_t>(found - all.begin()) + 1;
            retained.push_back(std::move(line));
        }
        if (input.bad()) return fail("patches file could not be read completely");
    }

    // Git's default switch/merge behavior may overwrite ignored content.
    // Inspect path bytes with -z, including file/directory prefix collisions.
    std::set<std::string> writtenPaths;
    const auto targetPaths = GitUpstream(repo, {"ls-tree", "-r", "--name-only", "-z", target});
    if (targetPaths.exitCode != 0) return fail("cannot inspect target paths");
    for (const auto& path : PathsUpstream(targetPaths.stdoutStr)) writtenPaths.insert(path);
    for (const auto& sha : retained) {
        const auto patchPaths = GitUpstream(repo, {"diff-tree", "--no-commit-id", "--name-only", "-r", "--no-renames", "-z", sha});
        if (patchPaths.exitCode != 0) return fail("cannot inspect retained patch paths");
        for (const auto& path : PathsUpstream(patchPaths.stdoutStr)) writtenPaths.insert(path);
    }
    const auto ignoredFailure = [&]() -> std::string {
        // Compress wholly ignored directories rather than enumerating vendor trees.
        const auto ignored = GitUpstream(repo, {"ls-files", "--others", "--ignored", "--exclude-standard", "--directory", "-z"});
        if (ignored.exitCode != 0) return "cannot inspect ignored paths";
        for (const auto& path : PathsUpstream(ignored.stdoutStr))
            for (const auto& written : writtenPaths)
                if (PathsOverlapUpstream(path, written))
                    return "an ignored path overlaps the target or retained patches; no files were overwritten";
        return {};
    };
    if (const auto reason = ignoredFailure(); !reason.empty()) return fail(reason);

    std::cout << "=== Upstream patch plan ===\n"
              << "source_branch: " << sourceBranch << "\nbase: " << base << "\nhead: " << head
              << "\ntarget: " << target << "\nbranch: " << options.branch << "\nretained:\n";
    for (const auto& sha : retained) std::cout << "  " << sha << '\n';
    std::cout << "dropped:\n";
    for (const auto& sha : all)
        if (std::find(retained.begin(), retained.end(), sha) == retained.end()) std::cout << "  " << sha << '\n';
    std::cout << std::flush;
    if (options.dryRun) return 0;

    // Recheck the selected source before the first mutation. All object inputs
    // below are pinned SHAs, never movable refs or arbitrary patch-file arguments.
    const auto freshStatus = GitUpstream(repo, {"status", "--porcelain=v1", "--untracked-files=normal"});
    const auto freshBranch = GitUpstream(repo, {"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (resolve("HEAD") != head || freshStatus.exitCode != 0 || !TrimUpstream(freshStatus.stdoutStr).empty() ||
        freshBranch.exitCode != 0 || TrimUpstream(freshBranch.stdoutStr) != sourceBranch)
        return fail("source changed after planning; no branch was created");
    if (const auto reason = ignoredFailure(); !reason.empty()) return fail(reason);
    const auto created = GitUpstream(repo, {"switch", "--no-overwrite-ignore", "-c", options.branch, target});
    if (created.exitCode != 0) {
        std::cerr << created.stderrStr;
        return fail("fresh branch creation failed; source was not rebased");
    }
    for (const auto& sha : retained) {
        const auto applied = GitUpstream(repo, {"cherry-pick", "-x", sha});
        if (applied.exitCode != 0) {
            std::cerr << applied.stdoutStr << applied.stderrStr
                      << "Patch replay stopped at " << sha << ". No rollback was attempted.\n"
                      << "Resolve and use: kog cherry-pick --continue --repo \"" << repo.generic_string() << "\"\n"
                      << "Or abort this cherry-pick: kog cherry-pick --abort --repo \"" << repo.generic_string() << "\"\n"
                      << "Then review the remaining retained commits from this plan.\n";
            return applied.exitCode;
        }
    }
    std::cout << "Upstream patch replay complete; source branch unchanged; nothing was pushed.\n";
    return 0;
}
} // namespace

auto MakeSyncUpstreamCommandCallback(CLI::App& command,
    const std::shared_ptr<SyncCommandOptions>& options) -> std::function<void()> {
    return [&command, optionsOwner = options]() {
        if (!command.remaining().empty()) {
            std::cerr << "ERROR: unsupported extra arguments in sync upstream\n";
            std::exit(2);
        }
        try { std::exit(RunUpstreamSync(optionsOwner->upstream)); }
        catch (const std::exception& error) {
            std::cerr << "ERROR: upstream sync: " << error.what() << '\n';
            std::exit(1);
        }
    };
}
} // namespace kano::git::commands
