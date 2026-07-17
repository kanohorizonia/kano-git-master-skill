#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto RequireContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("missing=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) != std::string::npos);
}

auto WriteText(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream stream(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << InText;
    stream.close();
    REQUIRE(stream.good());
}

auto InitRepo(const std::string& InName, const std::vector<std::string>& InFiles) -> std::pair<SandboxContext, std::filesystem::path> {
    auto sandbox = CreateSandboxWorkspace(InName);
    const auto repo = sandbox.root / "repo";
    std::filesystem::create_directories(repo);
    RequireSuccess(RunGit({"init", "-q"}, repo), "git init");
    RequireSuccess(RunGit({"config", "user.name", "KOG Test"}, repo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kog-test@example.invalid"}, repo), "config user.email");
    for (const auto& file : InFiles) {
        WriteText(repo / file, "seed " + file + "\n");
    }
    RequireSuccess(RunGit({"add", "."}, repo), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");
    return {std::move(sandbox), repo};
}

auto GitOutput(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> std::string {
    const auto result = RunGit(InArgs, InRepo);
    RequireSuccess(result, "git output");
    return result.stdoutText;
}

auto ExtractBatchId(const std::string& InOutput) -> std::string {
    const auto marker = std::string{"\"id\": \"batch-"};
    const auto begin = InOutput.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto valueBegin = begin + std::string{"\"id\": \""}.size();
    const auto end = InOutput.find('"', valueBegin);
    REQUIRE(end != std::string::npos);
    return InOutput.substr(valueBegin, end - valueBegin);
}

} // namespace

TEST_CASE("exact-path commit isolates add modify delete and rename from unrelated staged state",
          "[functional][KG-TSK-0112][exact-path]") {
    auto [sandbox, repo] = InitRepo("exact-path-mixed", {
        "modify.txt", "delete.txt", "rename-old.txt", "staged.txt", "excluded.txt",
    });
    WriteText(repo / "modify.txt", "modified\n");
    WriteText(repo / "add.txt", "added\n");
    std::filesystem::remove(repo / "delete.txt");
    std::filesystem::rename(repo / "rename-old.txt", repo / "rename-new.txt");
    WriteText(repo / "staged.txt", "unrelated staged\n");
    WriteText(repo / "excluded.txt", "unrelated unstaged\n");
    RequireSuccess(RunGit({"add", "staged.txt"}, repo), "stage unrelated file");
    const auto stagedBefore = GitOutput(repo, {"ls-files", "--stage", "staged.txt"});

    const auto result = RunKog({
        "commit", "--exact-path", "add.txt", "--exact-path", "modify.txt",
        "--exact-path", "delete.txt", "--exact-path", "rename-old.txt",
        "--exact-path", "rename-new.txt", "-m", "[Test][Chore] exact mixed",
    }, repo);
    RequireSuccess(result, "exact-path mixed commit");
    RequireContains(result.stdoutText, "\"status\": \"committed\"");
    RequireContains(result.stdoutText, "\"unrelatedStagedPreserved\": true");

    const auto committed = GitOutput(repo, {"diff-tree", "--no-commit-id", "--name-only", "-r", "--no-renames", "HEAD"});
    RequireContains(committed, "add.txt");
    RequireContains(committed, "modify.txt");
    RequireContains(committed, "delete.txt");
    RequireContains(committed, "rename-old.txt");
    RequireContains(committed, "rename-new.txt");
    REQUIRE(committed.find("staged.txt") == std::string::npos);
    REQUIRE(committed.find("excluded.txt") == std::string::npos);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", "staged.txt"}) == stagedBefore);
    const auto status = GitOutput(repo, {"status", "--short"});
    RequireContains(status, "M  staged.txt");
    RequireContains(status, " M excluded.txt");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("exact-path dry-run accepts explicit no-recursive without mutation",
          "[functional][KG-TSK-0112][KG-BUG-0058][KG-BUG-0061][dry-run]") {
    auto [sandbox, repo] = InitRepo("exact-path-preview", {"selected.txt", "excluded.txt"});
    WriteText(repo / "selected.txt", "selected\n");
    WriteText(repo / "excluded.txt", "excluded\n");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto statusBefore = GitOutput(repo, {"status", "--short"});
    const auto diagnosticsLog = (sandbox.root / "exact-path-preview-process.log").string();
    const auto result = RunKogWithEnv({
        "commit", "--no-recursive", "--no-ai-review", "--exact-path", "selected.txt", "-m", "[Test][Chore] preview", "--dry-run",
    }, repo, {{"KOG_PROCESS_DIAGNOSTICS_LOG", diagnosticsLog}});
    RequireSuccess(result, "exact-path dry-run");
    RequireContains(result.stdoutText, "\"status\": \"preview\"");
    RequireContains(result.stdoutText, "selected.txt");
    RequireContains(result.stdoutText, "excluded.txt");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"status", "--short"}) == statusBefore);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("exact-path accepts tracked gitlinks without broadening directory selectors",
          "[functional][KG-BUG-0049][exact-path][gitlink]") {
    auto [sandbox, repo] = InitRepo("exact-path-gitlink", {"excluded.txt", "ordinary/file.txt"});
    const auto childSource = sandbox.root / "child-source";
    std::filesystem::create_directories(childSource);
    RequireSuccess(RunGit({"init", "-q"}, childSource), "init child source");
    RequireSuccess(RunGit({"config", "user.name", "KOG Test"}, childSource), "config child user.name");
    RequireSuccess(RunGit({"config", "user.email", "kog-test@example.invalid"}, childSource), "config child user.email");
    WriteText(childSource / "child.txt", "seed child\n");
    RequireSuccess(RunGit({"add", "child.txt"}, childSource), "stage child seed");
    RequireSuccess(RunGit({"commit", "-m", "seed child"}, childSource), "commit child seed");
    RequireSuccess(RunGit({
        "-c", "protocol.file.allow=always", "submodule", "add", childSource.generic_string(), "vendor",
    }, repo), "add tracked gitlink");
    RequireSuccess(RunGit({"commit", "-am", "add tracked gitlink"}, repo), "commit tracked gitlink baseline");

    const auto ordinaryResult = RunKog({
        "commit", "--exact-path", "ordinary", "-m", "ordinary directory",
    }, repo);
    REQUIRE(ordinaryResult.exitCode != 0);
    RequireContains(ordinaryResult.stderrText, "invalid_exact_path");

    const auto child = repo / "vendor";
    RequireSuccess(RunGit({"config", "user.name", "KOG Test"}, child), "config cloned child user.name");
    RequireSuccess(RunGit({"config", "user.email", "kog-test@example.invalid"}, child), "config cloned child user.email");
    WriteText(child / "child.txt", "advanced child\n");
    RequireSuccess(RunGit({"add", "child.txt"}, child), "stage child advance");
    RequireSuccess(RunGit({"commit", "-m", "advance child"}, child), "commit child advance");
    WriteText(repo / "excluded.txt", "unrelated change\n");

    auto result = RunKog({
        "commit", "--exact-path", "vendor", "-m", "[Test][Chore] exact gitlink", "--dry-run",
    }, repo);
    RequireSuccess(result, "exact-path gitlink dry-run");
    RequireContains(result.stdoutText, "\"status\": \"preview\"");
    RequireContains(result.stdoutText, "vendor");

    result = RunKog({
        "commit", "--exact-path", "vendor", "-m", "[Test][Chore] exact gitlink",
    }, repo);
    RequireSuccess(result, "exact-path gitlink commit");
    RequireContains(result.stdoutText, "\"status\": \"committed\"");
    const auto committed = GitOutput(repo, {"diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"});
    RequireContains(committed, "vendor");
    REQUIRE(committed.find("excluded.txt") == std::string::npos);
    RequireContains(GitOutput(repo, {"status", "--short"}), " M excluded.txt");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("exact-path rejects outside overlap stale head and index lock without deleting the lock",
          "[functional][KG-TSK-0112][guards]") {
    auto [sandbox, repo] = InitRepo("exact-path-guards", {"dir/file.txt", "selected.txt"});
    WriteText(repo / "selected.txt", "changed\n");
    WriteText(sandbox.root / "outside.txt", "outside\n");

    auto result = RunKog({"commit", "--exact-path", "../outside.txt", "-m", "outside"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "invalid_exact_path");

    result = RunKog({
        "commit", "--exact-path", "dir", "--exact-path", "dir/file.txt", "-m", "overlap",
    }, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "overlapping path selectors");

    result = RunKog({
        "commit", "--exact-path", "selected.txt", "--expected-head",
        "0000000000000000000000000000000000000000", "-m", "stale",
    }, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "stale_base_head");

    const auto lockPath = repo / ".git" / "index.lock";
    WriteText(lockPath, "owned elsewhere\n");
    result = RunKog({"commit", "--exact-path", "selected.txt", "-m", "locked"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "git_index_lock");
    REQUIRE(std::filesystem::exists(lockPath));
    std::filesystem::remove(lockPath);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("agent queue merges disjoint chunks and constrains exact-path commit",
          "[functional][KG-TSK-0110][queue]") {
    auto [sandbox, repo] = InitRepo("agent-queue-compatible", {"shared.txt", "other.txt"});
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "item-a", "--work-item", "KG-1", "--agent", "codex-a",
        "--file", "shared.txt", "--chunk", "shared.txt:1-5", "--validate", "pixi run quick-test",
    }, repo), "admit first chunk");
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "item-b", "--work-item", "KG-2", "--agent", "codex-b",
        "--file", "shared.txt", "--chunk", "shared.txt:8-12",
    }, repo), "admit second chunk");
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "item-c", "--work-item", "KG-3", "--agent", "codex-c",
        "--file", "other.txt",
    }, repo), "admit disjoint file");

    auto result = RunKog({"agent-queue", "drain"}, repo);
    RequireSuccess(result, "preview compatible batch");
    RequireContains(result.stdoutText, "\"status\": \"preview\"");
    result = RunKog({"agent-queue", "drain", "--confirm"}, repo);
    RequireSuccess(result, "activate compatible batch");
    const auto batch = ExtractBatchId(result.stdoutText);

    WriteText(repo / "shared.txt", "queue committed\n");
    result = RunKog({
        "commit", "--exact-path", "shared.txt", "--queue-batch", batch,
        "-m", "[Test][Chore] queued exact commit",
    }, repo);
    RequireSuccess(result, "commit within active batch");
    RequireContains(result.stdoutText, batch);
    RequireSuccess(RunKog({
        "agent-queue", "complete", "--batch", batch, "--status", "succeeded",
    }, repo), "complete active batch");
    result = RunKog({"agent-queue", "status"}, repo);
    RequireSuccess(result, "queue status");
    RequireContains(result.stdoutText, "\"pendingCount\": 0");
    RequireContains(result.stdoutText, "\"activeCount\": 0");
    RequireContains(result.stdoutText, "\"status\": \"succeeded\"");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("agent queue fails closed on overlapping chunks and preserves pending items",
          "[functional][KG-TSK-0110][conflict]") {
    auto [sandbox, repo] = InitRepo("agent-queue-conflict", {"shared.txt"});
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "overlap-a", "--work-item", "KG-1", "--agent", "a",
        "--file", "shared.txt", "--chunk", "shared.txt:1-10",
    }, repo), "admit overlap a");
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "overlap-b", "--work-item", "KG-2", "--agent", "b",
        "--file", "shared.txt", "--chunk", "shared.txt:5-12",
    }, repo), "admit overlap b");
    auto result = RunKog({"agent-queue", "drain", "--confirm"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "queue_conflict");
    result = RunKog({"agent-queue", "status"}, repo);
    RequireSuccess(result, "status after conflict");
    RequireContains(result.stdoutText, "\"pendingCount\": 2");
    RequireContains(result.stdoutText, "\"activeCount\": 0");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("agent queue merges same-file intents with an identical postcondition",
          "[functional][KG-TSK-0110][postcondition]") {
    auto [sandbox, repo] = InitRepo("agent-queue-postcondition", {"shared.txt"});
    for (const auto& id : {"post-a", "post-b"}) {
        RequireSuccess(RunKog({
            "agent-queue", "admit", "--id", id, "--work-item", id, "--agent", id,
            "--file", "shared.txt", "--postcondition", "shared.txt=formatted",
        }, repo), "admit matching postcondition");
    }
    const auto result = RunKog({"agent-queue", "drain"}, repo);
    RequireSuccess(result, "preview matching postcondition");
    RequireContains(result.stdoutText, "\"status\": \"preview\"");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("agent queue blocks stale admissions without consuming pending intent",
          "[functional][KG-TSK-0110][stale]") {
    auto [sandbox, repo] = InitRepo("agent-queue-stale", {"queued.txt", "advance.txt"});
    RequireSuccess(RunKog({
        "agent-queue", "admit", "--id", "stale-item", "--work-item", "KG-1", "--agent", "codex",
        "--file", "queued.txt",
    }, repo), "admit before HEAD advance");
    WriteText(repo / "advance.txt", "advanced\n");
    RequireSuccess(RunGit({"add", "advance.txt"}, repo), "stage HEAD advance");
    RequireSuccess(RunGit({"commit", "-m", "advance head"}, repo), "advance HEAD");
    auto result = RunKog({"agent-queue", "drain", "--confirm"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "stale_base_head");
    result = RunKog({"agent-queue", "status"}, repo);
    RequireSuccess(result, "status after stale admission");
    RequireContains(result.stdoutText, "\"pendingCount\": 1");
    RequireContains(result.stdoutText, "\"activeCount\": 0");
    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
