#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

auto JsonArrayForKey(const std::string& InText, const std::string& InKey) -> std::string {
    const auto marker = "\"" + InKey + "\": [";
    const auto begin = InText.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto end = InText.find(']', begin + marker.size());
    REQUIRE(end != std::string::npos);
    return InText.substr(begin, end - begin + 1);
}

auto WriteText(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream stream(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << InText;
    stream.close();
    REQUIRE(stream.good());
}

auto ReadText(const std::filesystem::path& InPath) -> std::string {
    std::ifstream stream(InPath, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
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
          "[functional][KG-TSK-0112][KG-BUG-0063][exact-path]") {
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
    const auto includedReceipt = JsonArrayForKey(result.stdoutText, "included");
    const auto excludedReceipt = JsonArrayForKey(result.stdoutText, "excluded");
    RequireContains(includedReceipt, "rename-old.txt");
    RequireContains(includedReceipt, "rename-new.txt");
    REQUIRE(excludedReceipt.find("rename-old.txt") == std::string::npos);
    REQUIRE(excludedReceipt.find("rename-new.txt") == std::string::npos);
    RequireContains(excludedReceipt, "excluded.txt");

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

TEST_CASE("exact-path stages tracked deletions beneath a newly selected ignore rule",
          "[functional][exact-path][KG-BUG-0083]") {
    auto [sandbox, repo] = InitRepo("exact-path-ignored-tracked-deletion", {
        ".gitignore", ".kano/tmp/tracked.json", "staged.txt",
    });
    WriteText(repo / ".gitignore", ".kano/tmp/\n");
    std::filesystem::remove(repo / ".kano/tmp/tracked.json");
    WriteText(repo / "staged.txt", "unrelated staged\n");
    RequireSuccess(RunGit({"add", "staged.txt"}, repo), "stage unrelated file");
    const auto stagedBefore = GitOutput(repo, {"ls-files", "--stage", "staged.txt"});

    const auto result = RunKog({
        "commit", "--exact-path", ".gitignore",
        "--exact-path", ".kano/tmp/tracked.json",
        "-m", "[Test][BugFix] remove newly ignored tracked file",
    }, repo);
    RequireSuccess(result, "exact-path ignored tracked deletion commit");
    RequireContains(result.stdoutText, "\"status\": \"committed\"");
    const auto includedReceipt = JsonArrayForKey(result.stdoutText, "included");
    RequireContains(includedReceipt, ".gitignore");
    RequireContains(includedReceipt, ".kano/tmp/tracked.json");

    const auto committed = GitOutput(
        repo,
        {"diff-tree", "--no-commit-id", "--name-only", "-r", "--no-renames", "HEAD"});
    RequireContains(committed, ".gitignore");
    RequireContains(committed, ".kano/tmp/tracked.json");
    REQUIRE(committed.find("staged.txt") == std::string::npos);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", "staged.txt"}) == stagedBefore);

    WriteText(repo / ".kano/tmp/untracked.json", "{\"ignored\":true}\n");
    const auto headBeforeRejectedAdd = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto rejected = RunKog({
        "commit", "--exact-path", ".kano/tmp/untracked.json",
        "-m", "[Test][BugFix] must not force add ignored untracked file",
    }, repo);
    INFO(rejected.stdoutText);
    INFO(rejected.stderrText);
    REQUIRE(rejected.exitCode != 0);
    RequireContains(
        rejected.stdoutText + "\n" + rejected.stderrText,
        "\"blocker\": \"exact_stage_failed\"");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBeforeRejectedAdd);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", "staged.txt"}) == stagedBefore);

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

TEST_CASE("exact-path distinguishes queue contention from lock acquisition errors",
          "[functional][KG-BUG-0079][exact-path][queue-lock]") {
    auto [sandbox, repo] = InitRepo("exact-path-queue-lock-errors", {"selected.txt"});
    WriteText(repo / "selected.txt", "changed\n");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto selectedStatusBefore = GitOutput(repo, {"status", "--short", "--", "selected.txt"});
    const auto queueRoot = repo / ".git" / "kano-agent-queue";
    const auto lockPath = queueRoot / "mutation.lock";

    std::filesystem::create_directories(lockPath);
    auto result = RunKog({
        "commit", "--exact-path", "selected.txt", "-m", "[Test][Chore] active queue lock",
    }, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "\"blocker\": \"queue_locked\"");
    REQUIRE(std::filesystem::is_directory(lockPath));
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"status", "--short", "--", "selected.txt"}) == selectedStatusBefore);
    std::filesystem::remove_all(lockPath);

    WriteText(lockPath, "invalid lock path\n");
    result = RunKog({
        "commit", "--exact-path", "selected.txt", "-m", "[Test][Chore] invalid queue lock path",
    }, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "\"blocker\": \"queue_lock_failed\"");
    RequireContains(result.stderrText, "cannot create mutation lock directory");
    REQUIRE(std::filesystem::is_regular_file(lockPath));
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"status", "--short", "--", "selected.txt"}) == selectedStatusBefore);
    RemoveSandboxWorkspace(sandbox);
}

namespace {
auto VerifyCheckpointOwnCommit(const bool InCrlf, const bool InPrune, const bool InMissingAttributes = false,
                               const bool InWorkingAttributes = false) -> void {
    const std::string sharedPath = InWorkingAttributes ? "nested/shared.txt" : "shared.txt";
    auto [sandbox, repo] = InitRepo("cooperative-checkpoint", {sharedPath, "other.txt"});
    RequireSuccess(RunGit({"branch", "-m", "codex/cooperative-test"}, repo), "select owned branch");
    RequireSuccess(RunGit({"config", "core.autocrlf", InCrlf ? "true" : "false"}, repo), "configure checkout normalization");
    WriteText(repo / ".gitattributes", "*.txt text=auto\n");
    RequireSuccess(RunGit({"add", ".gitattributes"}, repo), "stage text attributes");
    RequireSuccess(RunGit({"commit", "-m", "text attributes"}, repo), "commit text attributes");
    if (InWorkingAttributes) {
        WriteText(repo / "nested/.gitattributes", "*.txt -text\n");
        RequireSuccess(RunGit({"add", "nested/.gitattributes"}, repo), "stage indexed nested attributes");
        RequireSuccess(RunGit({"commit", "-m", "indexed nested attributes"}, repo), "commit indexed nested attributes");
        WriteText(repo / "nested/.gitattributes", "*.txt text=auto\n");
    }
    const auto attributeStageBefore = GitOutput(repo, {"ls-files", "--stage", "--", ".gitattributes", "nested/.gitattributes"});
    const auto raw = [InCrlf](const std::string& bytes) {
        if (!InCrlf) return bytes;
        std::string result;
        for (const char ch : bytes) {
            if (ch == '\n') result += '\r';
            result += ch;
        }
        return result;
    };
    const std::string base = "base first\ncontext a\nbase middle\ncontext b\ncontext c\ncontext d\nbase end\n";
    const std::string staged = "pre-existing staged\ncontext a\nbase middle\ncontext b\ncontext c\ncontext d\nbase end\n";
    const std::string working = "pre-existing staged\ncontext a\npre-existing working\ncontext b\ncontext c\ncontext d\nbase end\n";
    const std::string final = "pre-existing staged\ncontext a\npre-existing working\ncontext b\ncontext c\ncontext d\nown change\n";
    const std::string own = "base first\ncontext a\nbase middle\ncontext b\ncontext c\ncontext d\nown change\n";
    const std::string restaged = "pre-existing staged\ncontext a\nbase middle\ncontext b\ncontext c\ncontext d\nown change\n";
    WriteText(repo / sharedPath, raw(base));
    RequireSuccess(RunGit({"add", sharedPath}, repo), "stage three-line base");
    RequireSuccess(RunGit({"commit", "-m", "three-line base"}, repo), "commit three-line base");
    WriteText(repo / sharedPath, raw(staged));
    RequireSuccess(RunGit({"add", sharedPath}, repo), "stage pre-existing first version");
    const auto stagedBefore = GitOutput(repo, {"ls-files", "--stage", sharedPath});
    WriteText(repo / sharedPath, raw(working));
    const auto workingBefore = ReadText(repo / sharedPath);
    REQUIRE(stagedBefore.find(sharedPath) != std::string::npos);

    std::vector<std::string> unrelatedStageBefore;
    for (int index = 0; index < 10; ++index) {
        const auto path = "unrelated-" + std::to_string(index) + ".txt";
        WriteText(repo / path, "staged " + std::to_string(index) + "\n");
        RequireSuccess(RunGit({"add", path}, repo), "stage unrelated version");
        if (index == 0) {
            RequireSuccess(RunGit({"update-index", "--chmod=+x", "--", path}, repo), "preserve an executable staged mode");
        }
        unrelatedStageBefore.push_back(GitOutput(repo, {"ls-files", "--stage", "--", path}));
        WriteText(repo / path, "working " + std::to_string(index) + "\n");
    }

    auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "overlap-one",
                          "--path", sharedPath, "--source", "pre-existing/unknown",
                          "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture pre-existing versions");
    RequireContains(result.stdoutText, "\"status\": \"saved\"");
    RequireContains(result.stdoutText, "\"restoreVerified\": true");
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", sharedPath}) == stagedBefore);
    REQUIRE(ReadText(repo / sharedPath) == workingBefore);

    auto rawOid = GitOutput(repo, {"hash-object", "--no-filters", "--", sharedPath});
    rawOid.erase(rawOid.find_first_of("\r\n"));
    if (InMissingAttributes) {
        std::filesystem::remove(repo / ".gitattributes");
        RequireSuccess(RunGit({"config", "core.autocrlf", "false"}, repo), "require indexed text attributes for normalization");
    }
    WriteText(repo / sharedPath, raw(final));
    if (InPrune) {
        RequireSuccess(RunGit({"gc", "--prune=now"}, repo), "prune after captured original file changes");
        REQUIRE(GitOutput(repo, {"cat-file", "blob", rawOid}) == workingBefore);
    }
    result = RunKog({"commit", "--exact-path", sharedPath, "--overlap-checkpoint", "overlap-one",
                     "-m", "[Test][Chore] own only (KOG-TSK-0142)"}, repo);
    RequireSuccess(result, "checkpoint then own-only commit");
    RequireContains(result.stdoutText, "\"status\": \"committed\"");
    RequireContains(result.stdoutText, "\"checkpointStatus\": \"saved\"");
    RequireContains(result.stdoutText, "\"published\": false");
    REQUIRE(GitOutput(repo, {"show", "HEAD:" + sharedPath}) == own);
    REQUIRE(GitOutput(repo, {"show", "HEAD^:" + sharedPath}) == base);
    REQUIRE(GitOutput(repo, {"show", "refs/kog/checkpoints/overlap-one:" + sharedPath}) == workingBefore);
    REQUIRE(GitOutput(repo, {"show", "refs/kog/checkpoints/overlap-one^:" + sharedPath}) == staged);
    REQUIRE(GitOutput(repo, {"show", ":" + sharedPath}) == restaged);
    REQUIRE(ReadText(repo / sharedPath) == raw(final));
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", sharedPath}) != stagedBefore);
    for (int index = 0; index < 10; ++index) {
        const auto path = "unrelated-" + std::to_string(index) + ".txt";
        RequireContains(GitOutput(repo, {"status", "--short", "--", path}), "AM " + path);
        REQUIRE(ReadText(repo / path) == "working " + std::to_string(index) + "\n");
        REQUIRE(GitOutput(repo, {"ls-files", "--stage", "--", path}) == unrelatedStageBefore[index]);
    }
    const auto unrelatedIndexAfter = GitOutput(repo, {"ls-files", "--stage"});
    REQUIRE(unrelatedIndexAfter.find("unrelated-0.txt") != std::string::npos);
    REQUIRE(unrelatedIndexAfter.find("unrelated-9.txt") != std::string::npos);
    REQUIRE(GitOutput(repo, {"rev-parse", "refs/kog/checkpoints/overlap-one"}) !=
            GitOutput(repo, {"rev-parse", "HEAD^"}));
    REQUIRE(RunGit({"merge-base", "--is-ancestor", "refs/kog/checkpoints/overlap-one", "HEAD"}, repo).exitCode != 0);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage", "--", ".gitattributes", "nested/.gitattributes"}) == attributeStageBefore);
    if (InMissingAttributes) REQUIRE_FALSE(std::filesystem::exists(repo / ".gitattributes"));
    if (InWorkingAttributes) REQUIRE(ReadText(repo / "nested/.gitattributes") == "*.txt text=auto\n");
    RemoveSandboxWorkspace(sandbox);
}
} // namespace

TEST_CASE("cooperative checkpoint preserves staged and working versions before an own-only commit",
          "[functional][KOG-TSK-0142][checkpoint]") {
    VerifyCheckpointOwnCommit(false, false);
}

TEST_CASE("cooperative checkpoint separates path-normalized LF and CRLF edits without changing raw WIP",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    VerifyCheckpointOwnCommit(GENERATE(false, true), false);
}

TEST_CASE("cooperative checkpoint captured blobs survive prune before own-only commit",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    VerifyCheckpointOwnCommit(GENERATE(false, true), true);
}

TEST_CASE("cooperative checkpoint uses index attributes when the working attributes file is deleted",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    VerifyCheckpointOwnCommit(true, false, true);
}

TEST_CASE("cooperative checkpoint preserves working ancestor attributes over indexed fallback",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    VerifyCheckpointOwnCommit(true, false, true, true);
}

TEST_CASE("cooperative checkpoint detects reference-hook writer drift before reporting saved",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    auto [sandbox, repo] = InitRepo("checkpoint-pin-writer", {"shared.txt"});
    WriteText(repo / "shared.txt", "captured WIP\n");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto indexBefore = GitOutput(repo, {"ls-files", "--stage"});
    const auto hook = repo / ".git/hooks/reference-transaction";
    WriteText(hook, "#!/bin/sh\nif [ \"$1\" = committed ]; then\n"
                    "  while read old new ref; do\n"
                    "    case \"$ref\" in refs/kog/checkpoint-snapshots/*/working/*) printf 'hook writer drift\\n' > shared.txt ;; esac\n"
                    "  done\nfi\n");
    std::filesystem::permissions(hook, std::filesystem::perms::owner_exec,
                                  std::filesystem::perm_options::add);
    const auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "hook-drift",
                               "--path", "shared.txt", "--source", "pre-existing/unknown",
                               "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "checkpoint_snapshot_drift");
    REQUIRE_FALSE(std::filesystem::exists(repo / ".git/kano-agent-queue/checkpoints/hook-drift/manifest.json"));
    REQUIRE(ReadText(repo / "shared.txt") == "hook writer drift\n");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage"}) == indexBefore);
    REQUIRE(GitOutput(repo, {"show", "refs/kog/checkpoint-snapshots/hook-drift/working/0"}) == "captured WIP\n");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint refuses secret clean-filter output without archiving it",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    const bool dryRun = GENERATE(false, true);
    auto [sandbox, repo] = InitRepo("checkpoint-filter-secret", {"shared.txt"});
    RequireSuccess(RunGit({"branch", "-m", "codex/filter-secret"}, repo), "owned branch");
    WriteText(repo / ".gitattributes", "shared.txt filter=opaque-clean\n");
    RequireSuccess(RunGit({"add", ".gitattributes"}, repo), "stage path filter");
    RequireSuccess(RunGit({"commit", "-m", "path filter"}, repo), "commit path filter");
    // Deliberately recognizable fake secret pattern; no credential is used.
    const std::string filterOutput = "api_key='abcdefghijklmnopqrstuvwxyz012345'\n";
    WriteText(repo / ".kano/tmp/clean-output.txt", filterOutput);
    auto oid = GitOutput(repo, {"hash-object", "--no-filters", "--", ".kano/tmp/clean-output.txt"});
    oid.erase(oid.find_first_of("\r\n"));
    RequireSuccess(RunGit({"config", "filter.opaque-clean.clean",
                          "printf \"api_key='abcdefghijklmnopqrstuvwxyz012345'\\n\""}, repo), "configure fake secret-producing filter");
    RequireSuccess(RunGit({"config", "filter.opaque-clean.required", "true"}, repo), "require filter");
    WriteText(repo / "shared.txt", "harmless pre-existing working text\n");
    RequireSuccess(RunKog({"agent-queue", "checkpoint", "capture", "--id", "filter-secret",
                           "--path", "shared.txt", "--source", "pre-existing/unknown",
                           "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo), "capture raw harmless WIP");
    WriteText(repo / "shared.txt", "harmless caller edit\n");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto indexBefore = GitOutput(repo, {"ls-files", "--stage"});
    const auto refsBefore = GitOutput(repo, {"show-ref"});
    const auto objectsBefore = GitOutput(repo, {"count-objects", "-v"});
    REQUIRE(RunGit({"cat-file", "-e", oid}, repo).exitCode != 0);
    std::vector<std::string> args{"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "filter-secret",
                                  "-m", "[Test][Chore] filter refusal (KOG-TSK-0142)"};
    if (dryRun) args.push_back("--dry-run");
    const auto result = RunKog(args, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "secret_detected");
    REQUIRE(RunGit({"cat-file", "-e", oid}, repo).exitCode != 0);
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage"}) == indexBefore);
    REQUIRE(GitOutput(repo, {"show-ref"}) == refsBefore);
    REQUIRE(GitOutput(repo, {"count-objects", "-v"}) == objectsBefore);
    REQUIRE(ReadText(repo / "shared.txt") == "harmless caller edit\n");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint pins base staged and raw working snapshots across destructive prune",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    auto [sandbox, repo] = InitRepo("checkpoint-all-snapshots", {"shared.txt"});
    RequireSuccess(RunGit({"branch", "-m", "codex/all-snapshots"}, repo), "owned branch");
    const auto trim = [](std::string value) { value.erase(value.find_first_of("\r\n")); return value; };
    const auto base = trim(GitOutput(repo, {"rev-parse", "HEAD"}));
    WriteText(repo / "shared.txt", "captured staged text\n");
    RequireSuccess(RunGit({"add", "shared.txt"}, repo), "stage captured snapshot");
    const auto staged = trim(GitOutput(repo, {"rev-parse", ":shared.txt"}));
    WriteText(repo / "shared.txt", "captured raw working text\r\n");
    const auto working = trim(GitOutput(repo, {"hash-object", "--no-filters", "--", "shared.txt"}));
    RequireSuccess(RunKog({"agent-queue", "checkpoint", "capture", "--id", "all-snapshots",
                           "--path", "shared.txt", "--source", "pre-existing/unknown",
                           "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo), "capture all versions");
    const std::string prefix = "refs/kog/checkpoint-snapshots/all-snapshots/";
    REQUIRE(trim(GitOutput(repo, {"rev-parse", prefix + "base"})) == base);
    REQUIRE(trim(GitOutput(repo, {"rev-parse", prefix + "staged/0"})) == staged);
    REQUIRE(trim(GitOutput(repo, {"rev-parse", prefix + "working/0"})) == working);
    // Disposable fixture only: make every captured version unreachable from
    // branch/index/reflog so the archival refs provide the sole retention.
    WriteText(repo / "shared.txt", "new unrelated root state\n");
    RequireSuccess(RunGit({"add", "shared.txt"}, repo), "replace fixture index snapshot");
    const auto tree = trim(GitOutput(repo, {"write-tree"}));
    const auto newRoot = trim(GitOutput(repo, {"commit-tree", tree, "-m", "fixture independent root"}));
    RequireSuccess(RunGit({"update-ref", "HEAD", newRoot, base}, repo), "replace fixture branch ancestry");
    RequireSuccess(RunGit({"reflog", "expire", "--expire=now", "--all"}, repo), "expire fixture reflogs");
    RequireSuccess(RunGit({"gc", "--prune=now"}, repo), "prune fixture unreachable objects");
    REQUIRE(GitOutput(repo, {"show", base + ":shared.txt"}) == "seed shared.txt\n");
    REQUIRE(GitOutput(repo, {"cat-file", "blob", staged}) == "captured staged text\n");
    REQUIRE(GitOutput(repo, {"cat-file", "blob", working}) == "captured raw working text\r\n");
    REQUIRE(ReadText(repo / "shared.txt") == "new unrelated root state\n");
    REQUIRE(GitOutput(repo, {"show", ":shared.txt"}) == "new unrelated root state\n");
    const auto result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "all-snapshots",
                               "-m", "[Test][Chore] reject drift (KOG-TSK-0142)"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "stale_base_head");
    REQUIRE(trim(GitOutput(repo, {"rev-parse", "HEAD"})) == newRoot);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint excludes root and nested sensitive directories before writing objects",
          "[functional][KOG-TSK-0142][checkpoint][checkpoint-repair]") {
    const auto sensitive = GENERATE(std::string{"secrets/token.bin"}, std::string{"credentials/service.dat"},
                                    std::string{"nested/secrets/token.bin"}, std::string{"nested/credentials/service.dat"});
    auto [sandbox, repo] = InitRepo("checkpoint-sensitive-directories", {"ordinary.txt"});
    // Opaque harmless fixture deliberately does not match a content secret rule.
    WriteText(repo / sensitive, std::string{"fixture\0opaque-value", 20});
    WriteText(repo / "ordinary.txt", "ordinary pre-existing working state\n");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    const auto indexBefore = GitOutput(repo, {"ls-files", "--stage"});
    const auto objectsBefore = GitOutput(repo, {"count-objects", "-v"});
    const auto refsBefore = GitOutput(repo, {"show-ref"});
    auto oid = GitOutput(repo, {"hash-object", "--no-filters", "--", sensitive});
    oid.erase(oid.find_first_of("\r\n"));
    REQUIRE(RunGit({"cat-file", "-e", oid}, repo).exitCode != 0);
    const auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "sensitive-dir",
                               "--path", "ordinary.txt", "--path", sensitive,
                               "--source", "pre-existing/unknown", "--work-item", "KOG-TSK-0142",
                               "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "excluded_checkpoint_path");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    REQUIRE(GitOutput(repo, {"ls-files", "--stage"}) == indexBefore);
    REQUIRE(GitOutput(repo, {"show-ref"}) == refsBefore);
    REQUIRE(GitOutput(repo, {"count-objects", "-v"}) == objectsBefore);
    REQUIRE(RunGit({"cat-file", "-e", oid}, repo).exitCode != 0);
    REQUIRE_FALSE(std::filesystem::exists(repo / ".git/kano-agent-queue/checkpoints/sensitive-dir/manifest.json"));
    REQUIRE(ReadText(repo / sensitive) == std::string{"fixture\0opaque-value", 20});
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint blocks binary split and same-hunk mixed ownership",
          "[functional][KOG-TSK-0142][checkpoint][guards]") {
    auto [sandbox, repo] = InitRepo("cooperative-checkpoint-guards", {"shared.txt", "asset.uasset"});
    RequireSuccess(RunGit({"branch", "-m", "codex/cooperative-guards"}, repo), "select owned branch");
    WriteText(repo / "shared.txt", "pre-existing\n");
    auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "mixed",
                          "--path", "shared.txt", "--source", "pre-existing/unknown",
                          "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture text overlap");
    const auto headBefore = GitOutput(repo, {"rev-parse", "HEAD"});
    WriteText(repo / "shared.txt", "own rewrite of pre-existing\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "mixed",
                     "-m", "[Test][Chore] mixed"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "inseparable_overlap");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);

    WriteText(repo / "asset.uasset", std::string{"binary\0state", 12});
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "binary",
                     "--path", "asset.uasset", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "save binary asset atomically");
    WriteText(repo / "asset.uasset", std::string{"binary\0newer", 12});
    result = RunKog({"commit", "--exact-path", "asset.uasset", "--overlap-checkpoint", "binary",
                     "-m", "[Test][Chore] binary"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "binary_overlap");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBefore);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint rejects secret lock and changed base without losing WIP",
          "[functional][KOG-TSK-0142][checkpoint][guards]") {
    auto [sandbox, repo] = InitRepo("cooperative-checkpoint-races", {"shared.txt", "other.txt"});
    RequireSuccess(RunGit({"branch", "-m", "codex/cooperative-races"}, repo), "select owned branch");
    WriteText(repo / "shared.txt", "api_key='abcdefghijklmnopqrstuvwxyz012345'\n");
    auto secretRawOid = GitOutput(repo, {"hash-object", "--no-filters", "--", "shared.txt"});
    secretRawOid.erase(secretRawOid.find_first_of("\r\n"));
    auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "secret",
                          "--path", "shared.txt", "--source", "pre-existing/unknown",
                          "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "secret_detected");
    REQUIRE_FALSE(std::filesystem::exists(repo / ".git" / "kano-agent-queue" / "checkpoints" / "secret" / "manifest.json"));
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/kog/checkpoints/secret"}, repo).exitCode != 0);
    REQUIRE(RunGit({"cat-file", "-e", secretRawOid}, repo).exitCode != 0);

    WriteText(repo / "shared.txt", "password='abcdefghijklmnopqrstuvwxyz012345'\n");
    RequireSuccess(RunGit({"add", "shared.txt"}, repo), "stage sensitive prior version");
    WriteText(repo / "shared.txt", "ordinary working version\n");
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "staged-secret",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "secret_detected");
    REQUIRE_FALSE(std::filesystem::exists(repo / ".git" / "kano-agent-queue" / "checkpoints" / "staged-secret" / "manifest.json"));
    RequireSuccess(RunGit({"reset", "-q", "HEAD", "--", "shared.txt"}, repo), "clear fixture-only staged secret");

    WriteText(repo / "shared.txt", "pre-existing\n");
    const auto queueLock = repo / ".git" / "kano-agent-queue" / "mutation.lock";
    std::filesystem::create_directories(queueLock);
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "active-writer",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "queue_locked");
    REQUIRE(std::filesystem::is_directory(queueLock));
    std::filesystem::remove(queueLock);
    const auto lockPath = repo / ".git" / "index.lock";
    WriteText(lockPath, "another writer\n");
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "locked",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "git_index_lock");
    REQUIRE(std::filesystem::exists(lockPath));
    std::filesystem::remove(lockPath);

    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "drift",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture before ref drift");
    WriteText(repo / "other.txt", "unrelated committed change\n");
    RequireSuccess(RunGit({"add", "other.txt"}, repo), "stage other");
    RequireSuccess(RunGit({"commit", "-m", "other change"}, repo), "advance HEAD");
    WriteText(repo / "shared.txt", "pre-existing\nown change\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "drift",
                     "-m", "[Test][Chore] stale"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "stale_base_head");
    REQUIRE(ReadText(repo / "shared.txt") == "pre-existing\nown change\n");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cooperative checkpoint keeps a diverged remote and a rejecting hook outside own history",
          "[functional][KOG-TSK-0142][checkpoint][guards]") {
    auto [sandbox, repo] = InitRepo("cooperative-checkpoint-divergence", {"shared.txt", "other.txt"});
    RequireSuccess(RunGit({"branch", "-m", "codex/cooperative-diverged"}, repo), "select owned branch");
    const std::string base = "base first\ncontext a\ncontext b\ncontext c\ncontext d\nbase last\n";
    WriteText(repo / "shared.txt", base);
    RequireSuccess(RunGit({"add", "shared.txt"}, repo), "stage base");
    RequireSuccess(RunGit({"commit", "-m", "base"}, repo), "commit base");
    const auto remote = sandbox.root / "remote.git";
    std::filesystem::create_directories(remote);
    RequireSuccess(RunGit({"init", "--bare", "-q"}, remote), "init bare remote");
    RequireSuccess(RunGit({"remote", "add", "origin", remote.string()}, repo), "add local remote");
    RequireSuccess(RunGit({"push", "-u", "origin", "codex/cooperative-diverged"}, repo), "publish fixture base");
    const auto peer = sandbox.root / "peer";
    RequireSuccess(RunGit({"clone", "-q", "-b", "codex/cooperative-diverged", remote.string(), peer.string()}, sandbox.root), "clone fixture peer");
    RequireSuccess(RunGit({"config", "user.name", "KOG Peer"}, peer), "peer name");
    RequireSuccess(RunGit({"config", "user.email", "kog-peer@example.invalid"}, peer), "peer email");
    WriteText(repo / "other.txt", "local-only commit\n");
    RequireSuccess(RunGit({"add", "other.txt"}, repo), "stage local-only");
    RequireSuccess(RunGit({"commit", "-m", "local-only"}, repo), "commit local-only");
    WriteText(peer / "remote-only.txt", "remote-only commit\n");
    RequireSuccess(RunGit({"add", "remote-only.txt"}, peer), "stage remote-only");
    RequireSuccess(RunGit({"commit", "-m", "remote-only"}, peer), "commit remote-only");
    RequireSuccess(RunGit({"push", "origin", "codex/cooperative-diverged"}, peer), "advance remote");
    RequireSuccess(RunGit({"fetch", "origin"}, repo), "observe divergent remote");
    const auto remoteBefore = GitOutput(repo, {"rev-parse", "origin/codex/cooperative-diverged"});
    RequireContains(GitOutput(repo, {"rev-list", "--left-right", "--count", "origin/codex/cooperative-diverged...HEAD"}), "1\t1");

    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nbase last\n");
    auto result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "diverged",
                          "--path", "shared.txt", "--source", "pre-existing/unknown",
                          "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture while remote diverges");
    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nown last\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "diverged",
                     "-m", "[Test][Chore] own in diverged branch (KOG-TSK-0142)"}, repo);
    RequireSuccess(result, "own local commit while remote diverges");
    REQUIRE(GitOutput(repo, {"rev-parse", "origin/codex/cooperative-diverged"}) == remoteBefore);
    REQUIRE(GitOutput(repo, {"show", "HEAD:shared.txt"}) == "base first\ncontext a\ncontext b\ncontext c\ncontext d\nown last\n");
    REQUIRE(RunGit({"merge-base", "--is-ancestor", "refs/kog/checkpoints/diverged", "HEAD"}, repo).exitCode != 0);
    RequireSuccess(RunGit({"push", "origin", "HEAD:refs/heads/codex/cooperative-candidate"}, repo), "publish only fixture feature branch");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/kog/checkpoints/diverged"}, remote).exitCode != 0);

    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nown last\n");
    const auto hook = repo / ".git" / "hooks" / "pre-commit";
    WriteText(hook, "#!/bin/sh\nexit 31\n");
    std::error_code ec;
    std::filesystem::permissions(hook, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add, ec);
    REQUIRE_FALSE(ec);
    const auto headBeforeHook = GitOutput(repo, {"rev-parse", "HEAD"});
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "hook-reject",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture before rejecting hook");
    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nnewer own last\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "hook-reject",
                     "-m", "[Test][Chore] must reject hook"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "checkpoint_commit_failed");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBeforeHook);
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/kog/checkpoints/hook-reject"}, repo).exitCode != 0);
    REQUIRE(std::filesystem::exists(repo / ".git" / "kano-agent-queue" / "checkpoints" / "hook-reject" / "manifest.json"));

    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nown last\n");
    WriteText(hook, "#!/bin/sh\nprintf 'hook injected\\n' > hook-added.txt\ngit add hook-added.txt\n");
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "hook-checkpoint-mutate",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture before checkpoint hook mutation");
    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\neven newer own last\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "hook-checkpoint-mutate",
                     "-m", "[Test][Chore] reject unexpected checkpoint tree"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "checkpoint_commit_failed");
    RequireContains(result.stderrText, "inspect preserved checkpoint checkout");
    REQUIRE(GitOutput(repo, {"rev-parse", "HEAD"}) == headBeforeHook);
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/kog/checkpoints/hook-checkpoint-mutate"}, repo).exitCode != 0);

    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nown last\n");
    WriteText(hook, "#!/bin/sh\nif git symbolic-ref -q HEAD >/dev/null 2>&1; then\n"
                    "  printf 'hook injected\\n' > hook-added.txt\n  git add hook-added.txt\nfi\n");
    result = RunKog({"agent-queue", "checkpoint", "capture", "--id", "hook-own-mutate",
                     "--path", "shared.txt", "--source", "pre-existing/unknown",
                     "--work-item", "KOG-TSK-0142", "--owner-stable"}, repo);
    RequireSuccess(result, "capture before own hook mutation");
    WriteText(repo / "shared.txt", "pre-existing\ncontext a\ncontext b\ncontext c\ncontext d\nultimate own last\n");
    result = RunKog({"commit", "--exact-path", "shared.txt", "--overlap-checkpoint", "hook-own-mutate",
                     "-m", "[Test][Chore] report unexpected own tree"}, repo);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stderrText, "\"status\": \"recovery_required\"");
    RequireContains(result.stderrText, "own_commit_readback_failed");
    REQUIRE(GitOutput(repo, {"show", "HEAD:hook-added.txt"}) == "hook injected\n");
    REQUIRE(GitOutput(repo, {"rev-parse", "refs/kog/recovery/hook-own-mutate"}) ==
            GitOutput(repo, {"rev-parse", "HEAD"}));
    REQUIRE(RunGit({"merge-base", "--is-ancestor", "refs/kog/checkpoints/hook-own-mutate", "HEAD"}, repo).exitCode != 0);
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
