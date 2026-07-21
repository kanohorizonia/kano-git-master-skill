#include "functional_test_support.hpp"
#include "../../../systems/kano_git_command/repo_sync/public/sync_output_sanitizer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

struct RepoRemote {
    std::filesystem::path bare;
    std::filesystem::path seed;
    std::filesystem::path clone;
    std::string branch{"main"};
};

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto RequireFailure(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode != 0);
}

auto RequireContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("missing needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) != std::string::npos);
}

auto RequireNotContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("unexpected needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) == std::string::npos);
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto TrimCopy(const std::string& InValue) -> std::string {
    const auto start = InValue.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(start, end - start + 1);
}

auto StripAnsi(const std::string& InText) -> std::string {
    std::string out;
    out.reserve(InText.size());
    bool inEsc = false;
    for (const char ch : InText) {
        if (!inEsc) {
            if (ch == '\x1b') {
                inEsc = true;
                continue;
            }
            out.push_back(ch);
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            inEsc = false;
        }
    }
    return out;
}

auto NormalizeLineEndings(const std::string& InText) -> std::string {
    std::string out;
    out.reserve(InText.size());
    for (std::size_t i = 0; i < InText.size(); ++i) {
        if (InText[i] == '\r') {
            if ((i + 1) < InText.size() && InText[i + 1] == '\n') {
                continue;
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(InText[i]);
    }
    return out;
}

auto CurrentHeadSha(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"rev-parse", "HEAD"}, InRepo);
    RequireSuccess(result, "rev-parse HEAD");
    return TrimCopy(result.stdoutText);
}

auto RefSha(const std::filesystem::path& InRepo, const std::string& InRef) -> std::string {
    const auto result = RunGit({"rev-parse", InRef}, InRepo);
    RequireSuccess(result, "rev-parse ref");
    return TrimCopy(result.stdoutText);
}

auto CommitFile(const std::filesystem::path& InRepo, const std::string& InRelativePath, const std::string& InText, const std::string& InMessage) -> std::string {
    WriteTextFile(InRepo / std::filesystem::path(InRelativePath), InText);
    RequireSuccess(RunGit({"add", InRelativePath}, InRepo), "add changed file");
    RequireSuccess(RunGit({"commit", "-m", InMessage}, InRepo), "commit changed file");
    return CurrentHeadSha(InRepo);
}

auto CreateRemote(const SandboxContext& InSandbox, const std::string& InName, const std::string& InBranch = "main") -> RepoRemote {
    RepoRemote out;
    out.branch = InBranch;
    out.bare = (InSandbox.root / (InName + "-remote.git")).lexically_normal();
    out.seed = (InSandbox.root / (InName + "-seed")).lexically_normal();
    out.clone = (InSandbox.root / (InName + "-clone")).lexically_normal();

    RequireSuccess(RunGit({"init", "--bare", out.bare.string()}, InSandbox.root), "init bare remote");
    RequireSuccess(RunGit({"init", out.seed.string()}, InSandbox.root), "init seed repo");
    ConfigureIdentity(out.seed);
    RequireSuccess(RunGit({"checkout", "-b", out.branch}, out.seed), "checkout seed branch");
    WriteTextFile(out.seed / ".gitignore", ".kano/\n");
    WriteTextFile(out.seed / "README.md", InName + " seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, out.seed), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed " + InName}, out.seed), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", out.bare.string()}, out.seed), "seed add origin");
    RequireSuccess(RunGit({"push", "-u", "origin", out.branch}, out.seed), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + out.branch}, out.bare), "set bare HEAD");
    RequireSuccess(RunGit({"clone", out.bare.string(), out.clone.string()}, InSandbox.root), "clone remote");
    ConfigureIdentity(out.clone);
    return out;
}

auto AddSubmodule(const std::filesystem::path& InParent,
                  const RepoRemote& InChild,
                  const std::string& InPath,
                  const std::string& InExtraPolicy = {}) -> std::filesystem::path {
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", InChild.branch, InChild.bare.string(), InPath}, InParent),
        "add submodule");
    if (!InExtraPolicy.empty()) {
        std::ofstream out(InParent / ".gitmodules", std::ios::binary | std::ios::app);
        REQUIRE(out.good());
        out << InExtraPolicy;
    }
    RequireSuccess(RunGit({"commit", "-am", "add submodule " + InPath}, InParent), "commit submodule add");
    const auto childPath = (InParent / std::filesystem::path(InPath)).lexically_normal();
    ConfigureIdentity(childPath);
    return childPath;
}

auto RunSyncRecursive(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"sync", "origin-latest"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto NormalizedDryRun(const std::string& InText) -> std::string {
    std::vector<std::string> lines;
    std::istringstream iss(StripAnsi(InText));
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("Discover mode:") || line.starts_with("Syncing workspace")) {
            lines.push_back(line);
            continue;
        }
        if (line.find("Repo:") != std::string::npos ||
            line.find("Taxonomy:") != std::string::npos ||
            line.find("Branch source:") != std::string::npos ||
            line.find("Would run:") != std::string::npos ||
            line.find("Skip rebase:") != std::string::npos ||
            line.find("SUMMARY:") != std::string::npos ||
            line.find("Succeeded:") != std::string::npos ||
            line.find("Skipped:") != std::string::npos ||
            line.find("Blocked:") != std::string::npos ||
            line.find("Failed:") != std::string::npos) {
            lines.push_back(line);
        }
    }
    std::ostringstream out;
    for (const auto& kept : lines) {
        out << kept << '\n';
    }
    return out.str();
}

auto PositionOf(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    const auto pos = InText.find(InNeedle);
    INFO("needle=" << InNeedle);
    INFO(InText);
    REQUIRE(pos != std::string::npos);
    return pos;
}

auto CountOccurrences(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    if (InNeedle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while (true) {
        pos = InText.find(InNeedle, pos);
        if (pos == std::string::npos) {
            break;
        }
        count += 1;
        pos += InNeedle.size();
    }
    return count;
}

} // namespace

TEST_CASE("recursive_sync_explicit_serial_and_parallel_policies_are_deterministic", "[functional][sync][recursive][determinism][policy]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-determinism");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childA = CreateRemote(sandbox, "child-a");
    auto childB = CreateRemote(sandbox, "child-b");
    const auto root = rootRemote.clone;
    AddSubmodule(root, childA, "deps/a", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    AddSubmodule(root, childB, "deps/b", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push registration");
    CommitFile(childA.seed, "remote-a.txt", "remote a\n", "remote update a");
    RequireSuccess(RunGit({"push", "origin", childA.branch}, childA.seed), "push child a remote update");
    CommitFile(childB.seed, "remote-b.txt", "remote b\n", "remote update b");
    RequireSuccess(RunGit({"push", "origin", childB.branch}, childB.seed), "push child b remote update");

    const auto serial = RunSyncRecursive(root, {"--dry-run", "--native-no-cache", "--execution-policy", "serial", "--jobs", "4"});
    const auto parallel = RunSyncRecursive(root, {"--dry-run", "--native-no-cache", "--execution-policy", "parallel", "--jobs", "4"});
    const auto serialOutput = StripAnsi(serial.stdoutText + "\n" + serial.stderrText);
    const auto parallelOutput = StripAnsi(parallel.stdoutText + "\n" + parallel.stderrText);
    RequireSuccess(serial, "sync dry-run explicit serial policy");
    RequireSuccess(parallel, "sync dry-run explicit parallel policy");
    RequireContains(serialOutput, "[native-sync] plan: repos=3 waves=2 order=child-first policy=serial jobs=1");
    RequireContains(serialOutput, "[native-sync] wave 1/2: deps/a deps/b");
    RequireContains(parallelOutput, "[native-sync] plan: repos=3 waves=2 order=child-first policy=parallel jobs=4");
    RequireContains(parallelOutput, "[native-sync] wave 1/2: deps/a deps/b");
    REQUIRE(NormalizedDryRun(serialOutput) == NormalizedDryRun(parallelOutput));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_rejects_unknown_execution_policy", "[functional][sync][recursive][policy][validation]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-invalid-policy");
    auto rootRemote = CreateRemote(sandbox, "root");

    const auto result = RunSyncRecursive(rootRemote.clone, {"--dry-run", "--execution-policy", "speculative"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "sync rejects unsupported execution policy");
    RequireContains(output, "Unsupported --execution-policy: speculative");
    RequireContains(output, "supported: serial, parallel");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_parallel_output_blocks_are_not_corrupted", "[functional][sync][recursive][determinism][output]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-output-stability");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childA = CreateRemote(sandbox, "child-a");
    auto childB = CreateRemote(sandbox, "child-b");
    const auto root = rootRemote.clone;
    AddSubmodule(root, childA, "deps/a", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    AddSubmodule(root, childB, "deps/b", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push registration");

    const auto result = RunSyncRecursive(root, {"--jobs", "4", "--dry-run"});
    const auto output = NormalizeLineEndings(StripAnsi(result.stdoutText + "\n" + result.stderrText));
    RequireSuccess(result, "sync output stability jobs=4");
    REQUIRE(output.find("(origin, main)\n(origin, main)") == std::string::npos);
    REQUIRE(output.find("\nn)\n") == std::string::npos);
    REQUIRE(output.find("\n/backlog] ") == std::string::npos);
    REQUIRE(CountOccurrences(output, "Repo: .") == 1);
    REQUIRE(CountOccurrences(output, "Repo: deps/a") == 1);
    REQUIRE(CountOccurrences(output, "Repo: deps/b") == 1);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_with_command_logging_env_keeps_repo_output_associated", "[functional][sync][recursive][determinism][logging]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-command-log-capture");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childA = CreateRemote(sandbox, "child-a");
    auto childB = CreateRemote(sandbox, "child-b");
    const auto root = rootRemote.clone;
    AddSubmodule(root, childA, "deps/a", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    AddSubmodule(root, childB, "deps/b", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push registration");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--dry-run", "--jobs", "4"},
        root,
        {
            {"KOG_LOG_COMMANDS", "1"},
            {"KOG_DEBUG", "1"},
            {"KANO_AGENT_MODE", "1"},
        });
    const auto output = NormalizeLineEndings(StripAnsi(result.stdoutText + "\n" + result.stderrText));
    RequireSuccess(result, "sync with command logging env");
    REQUIRE(output.find("[run] git") != std::string::npos);
    REQUIRE(output.find("(origin, main)\n(origin, main)") == std::string::npos);
    REQUIRE(output.find("\n/backlog] ") == std::string::npos);
    REQUIRE(CountOccurrences(output, "Repo: deps/a") == 1);
    REQUIRE(CountOccurrences(output, "Repo: deps/b") == 1);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_dependency_waves_update_child_before_parent", "[functional][sync][recursive][waves]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-waves");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/child", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push registration");
    const auto childRemoteHead = CommitFile(childRemote.seed, "child-remote.txt", "child remote update\n", "child remote update");
    RequireSuccess(RunGit({"push", "origin", childRemote.branch}, childRemote.seed), "push child remote update");

    const auto result = RunSyncRecursive(root, {"--jobs", "2"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive sync waves");
    RequireContains(output, "[native-sync] plan: repos=2 waves=2 order=child-first");
    RequireContains(output, "[native-sync] wave 1/2: deps/child");
    RequireContains(output, "[native-sync] wave 2/2: .");
    REQUIRE(PositionOf(output, "Repo: deps/child") < PositionOf(output, "Repo: ."));
    REQUIRE(CurrentHeadSha(childPath) == childRemoteHead);
    RequireContains(output, "SUMMARY: repos=2");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_kog_sync_false_is_visible_policy_skip", "[functional][sync][recursive][kog-sync-false]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-kog-sync-false");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/no-sync", "\tkog-sync = false\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push policy registration");
    const auto beforeChildHead = CurrentHeadSha(childPath);
    const auto childRemoteHead = CommitFile(childRemote.seed, "remote.txt", "remote should not sync\n", "remote update ignored by sync policy");
    RequireSuccess(RunGit({"push", "origin", childRemote.branch}, childRemote.seed), "push remote update");
    REQUIRE(childRemoteHead != beforeChildHead);

    const auto result = RunSyncRecursive(root, {"--jobs", "2"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive sync kog-sync=false");
    RequireContains(output, "deps/no-sync");
    RequireContains(output, "SKIPPED_BY_POLICY");
    RequireContains(output, "commandPolicy.sync=false / kog-sync=false");
    RequireContains(output, "skipped=1");
    REQUIRE(CurrentHeadSha(childPath) == beforeChildHead);

    const auto pushResult = RunKog({"push", "--recursive", "--bottom-up", "--jobs", "1"}, root);
    const auto pushOutput = StripAnsi(pushResult.stdoutText + "\n" + pushResult.stderrText);
    RequireSuccess(pushResult, "recursive push still evaluates independent kog-push policy");
    RequireContains(pushOutput, "commandPolicy.push=false / kog-push=false");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_conflict_is_best_effort_and_blocks_ancestor_only", "[functional][sync][recursive][conflict][best-effort]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-conflict-best-effort");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto conflictRemote = CreateRemote(sandbox, "conflict-child");
    auto healthyRemote = CreateRemote(sandbox, "healthy-child");
    const auto root = rootRemote.clone;
    const auto conflictPath = AddSubmodule(root, conflictRemote, "deps/conflict", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    const auto healthyPath = AddSubmodule(root, healthyRemote, "deps/healthy", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push sibling registration");

    const auto healthyRemoteHead = CommitFile(healthyRemote.seed, "healthy.txt", "healthy remote update\n", "healthy remote update");
    RequireSuccess(RunGit({"push", "origin", healthyRemote.branch}, healthyRemote.seed), "push healthy remote update");
    CommitFile(conflictRemote.seed, "README.md", "remote conflicting line\n", "remote conflicting update");
    RequireSuccess(RunGit({"push", "origin", conflictRemote.branch}, conflictRemote.seed), "push conflict remote update");
    const auto conflictLocalHead = CommitFile(conflictPath, "README.md", "local conflicting line\n", "local conflicting update");

    const auto result = RunSyncRecursive(root, {"--jobs", "2"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "recursive sync conflict best effort");
    RequireContains(output, "SYNC_CONFLICT");
    RequireContains(output, "BLOCKED_BY_CHILD_FAILURE");
    RequireContains(output, "FAILED REPOS");
    RequireContains(output, "BLOCKED REPOS");
    RequireContains(output, "deps/healthy");
    REQUIRE(CurrentHeadSha(healthyPath) == healthyRemoteHead);
    REQUIRE(CurrentHeadSha(conflictPath) == conflictLocalHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_rebases_non_conflicting_diverged_repo", "[functional][sync][recursive][diverged][rebase]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-diverged-rebase");
    auto rootRemote = CreateRemote(sandbox, "root");
    const auto root = rootRemote.clone;

    const auto localHead = CommitFile(root, "local.txt", "local commit\n", "local diverged commit");
    const auto remoteHead = CommitFile(rootRemote.seed, "remote.txt", "remote commit\n", "remote diverged commit");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, rootRemote.seed), "push diverged remote update");

    const auto result = RunSyncRecursive(root, {"--jobs", "1"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive sync rebase diverged repo");
    RequireContains(output, "continuing with fetch/rebase sync flow");
    RequireContains(output, "[.] SYNCED (origin, main)");
    REQUIRE(CurrentHeadSha(root) != localHead);
    REQUIRE(RefSha(root, "origin/main") == remoteHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_detached_head_with_local_commits_is_recovered_and_replayed", "[functional][sync][recursive][detached][replay]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-detached-replay");
    auto rootRemote = CreateRemote(sandbox, "root");
    const auto root = rootRemote.clone;

    const auto detachTarget = CurrentHeadSha(root);
    RequireSuccess(RunGit({"checkout", "--detach", detachTarget}, root), "detach HEAD for replay scenario");
    const auto detachedHead = CommitFile(root, "detached.txt", "detached commit\n", "detached local commit");

    const auto result = RunSyncRecursive(root, {"--jobs", "1"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive sync should recover detached local commits");
    RequireContains(output, "DETACHED_HEAD_UNSAFE_LOCAL_COMMITS");
    const bool replayed = output.find("DETACHED_HEAD_REPLAY: replayed") != std::string::npos;
    const bool replayNotNeeded = output.find("DETACHED_HEAD_REPLAY: no detached commits needed replay") != std::string::npos;
    REQUIRE((replayed || replayNotNeeded));
    RequireNotContains(output, "BLOCKED_PRECHECK: DETACHED_HEAD_UNSAFE_LOCAL_COMMITS");
    RequireContains(output, "[.] SYNCED (origin, main)");

    const auto branchOut = RunGit({"rev-parse", "--abbrev-ref", "HEAD"}, root);
    RequireSuccess(branchOut, "resolve current branch after sync detached recovery");
    REQUIRE(TrimCopy(branchOut.stdoutText) == "main");
    REQUIRE(CurrentHeadSha(root) != detachTarget);
    RequireSuccess(RunGit({"merge-base", "--is-ancestor", detachedHead, "HEAD"}, root), "detached commit should be reachable from current branch");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_ignores_non_selected_remote_fetch_failures", "[functional][sync][remote-selection]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-remote-selection-origin");
    auto rootRemote = CreateRemote(sandbox, "root");
    const auto root = rootRemote.clone;

    RequireSuccess(RunGit({"remote", "add", "gitlab_local", "file:///missing/path/for/fetch"}, root), "add broken non-selected remote");

    const auto result = RunSyncRecursive(root, {"--jobs", "1"});
    const auto output = NormalizeLineEndings(StripAnsi(result.stdoutText + "\n" + result.stderrText));
    RequireSuccess(result, "sync should use origin only");
    RequireContains(output, "Repo: .");
    RequireContains(output, "Taxonomy: root");
    RequireContains(output, "Selected remote: origin");
    RequireContains(output, "Repo: .\nTaxonomy:");
    REQUIRE(output.find("remote=gitlab_local fetch failed") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("sync_output_sanitizer_preserves_newlines", "[functional][sync][output][sanitize]") {
    using kano::git::commands::NormalizeSyncCapturedText;
    using kano::git::commands::SyncOutputSanitizeMode;

    REQUIRE(
        NormalizeSyncCapturedText("Repo: Fonts\r\nTaxonomy: registered\r\n", SyncOutputSanitizeMode::Human)
        == "Repo: Fonts\nTaxonomy: registered\n");

    REQUIRE(
        NormalizeSyncCapturedText("Repo: Fonts\rTaxonomy: registered\r", SyncOutputSanitizeMode::Human)
        == "Repo: Fonts\nTaxonomy: registered\n");

    const auto human = NormalizeSyncCapturedText(std::string("Repo:\x1b[2m Fonts\x1b[0m\n"), SyncOutputSanitizeMode::Human);
    REQUIRE(human == "Repo: Fonts\n");
    REQUIRE(human.find("\x1b") == std::string::npos);
}

TEST_CASE("recursive_sync_repo_context_uses_real_newlines_without_visible_control_glyphs", "[functional][sync][output][glyph-regression]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-visible-control-glyph-regression");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto fontsRemote = CreateRemote(sandbox, "fonts");
    const auto root = rootRemote.clone;
    AddSubmodule(root, fontsRemote, "Fonts", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push Fonts registration");

    const auto result = RunSyncRecursive(root, {"--jobs", "2"});
    const auto mergedRaw = result.stdoutText + "\n" + result.stderrText;
    const auto output = NormalizeLineEndings(StripAnsi(mergedRaw));
    RequireSuccess(result, "sync output should be human-readable");

    const auto repoLine = PositionOf(output, "Repo: Fonts\n");
    const auto taxonomyLine = output.find("Taxonomy: registered\n", repoLine);
    const auto branchSourceLine = output.find("Branch source: ", taxonomyLine);
    const auto selectedRemoteLine = output.find("Selected remote: origin\n", branchSourceLine);
    const auto sourceLine = output.find("Remote selection source: explicit policy\n", selectedRemoteLine);
    REQUIRE(taxonomyLine != std::string::npos);
    REQUIRE(branchSourceLine != std::string::npos);
    REQUIRE(selectedRemoteLine != std::string::npos);
    REQUIRE(sourceLine != std::string::npos);

    RequireNotContains(mergedRaw, "♪");
    RequireNotContains(mergedRaw, "◙");
    RequireNotContains(mergedRaw, "←[");
    RequireNotContains(output, "FontsTaxonomy");
    RequireNotContains(output, "Repo: Fonts♪");
    RequireNotContains(output, "♪◙Taxonomy");
    RequireNotContains(output, "Remote selection sourc◙");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_sync_repo_context_remains_clean_with_debug_logging_env", "[functional][sync][output][glyph-regression][env]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-sync-visible-control-glyph-env");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto fontsRemote = CreateRemote(sandbox, "fonts");
    const auto root = rootRemote.clone;
    AddSubmodule(root, fontsRemote, "Fonts", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push Fonts registration env");

    const auto result = RunKogWithEnv(
        {"sync", "origin-latest", "--jobs", "2"},
        root,
        {
            {"KOG_FORCE_COLOR", "1"},
            {"KOG_LOG_COMMANDS", "1"},
            {"KOG_DEBUG", "1"},
            {"KANO_AGENT_MODE", "1"},
        });
    const auto mergedRaw = result.stdoutText + "\n" + result.stderrText;
    const auto output = NormalizeLineEndings(StripAnsi(mergedRaw));
    RequireSuccess(result, "sync output remains clean with debug/log env");
    const auto repoLine = PositionOf(output, "Repo: Fonts\n");
    const auto taxonomyLine = output.find("Taxonomy: registered\n", repoLine);
    REQUIRE(taxonomyLine != std::string::npos);
    RequireNotContains(mergedRaw, "♪");
    RequireNotContains(mergedRaw, "◙");
    RequireNotContains(mergedRaw, "←[");

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
