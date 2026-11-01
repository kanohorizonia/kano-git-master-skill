#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kano::git::tests::functional {
namespace {

using RepositoryFiles = std::map<std::string, std::string>;

void RequireSuccess(const CommandResult& InResult, const std::string& InContext) {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

void RequireFailure(const CommandResult& InResult) {
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode != 0);
}

std::string Trim(const std::string& InValue) {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return InValue.substr(first, InValue.find_last_not_of(" \t\r\n") - first + 1);
}

void WriteFile(const std::filesystem::path& InPath, const std::string& InBytes) {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << InBytes;
    REQUIRE(output.good());
}

std::string ReadFile(const std::filesystem::path& InPath) {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string GitOutput(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) {
    const auto result = RunGit(InArgs, InRepo);
    RequireSuccess(result, "read fixture Git state");
    return Trim(result.stdoutText);
}

void Contains(const std::string& InText, const std::string& InExpected) {
    INFO("expected=" << InExpected);
    INFO(InText);
    REQUIRE(InText.find(InExpected) != std::string::npos);
}

RepositoryFiles CaptureFiles(const std::filesystem::path& InRepo) {
    RepositoryFiles files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(InRepo)) {
        if (!entry.is_regular_file()) continue;
        files.emplace(entry.path().lexically_relative(InRepo).generic_string(), ReadFile(entry.path()));
    }
    return files;
}

void RequireUnchanged(const std::filesystem::path& InRepo, const RepositoryFiles& InBefore) {
    const auto after = CaptureFiles(InRepo);
    REQUIRE(after.size() == InBefore.size());
    for (const auto& [path, bytes] : InBefore) {
        INFO("repository file=" << path);
        const auto found = after.find(path);
        REQUIRE(found != after.end());
        REQUIRE(found->second == bytes);
    }
}

struct UpstreamFixture {
    SandboxContext sandbox = CreateSandboxWorkspace("sync-upstream");
    std::filesystem::path repo = sandbox.root / "repo";
    const std::string sourceBranch = "kano/dsh-v0.2.1-rc.1";
    const std::string freshBranch = "codex/dsh-v0.2.1-rc.2";
    const std::string oldTag = "dsh-v0.2.1-rc.1";
    const std::string newTag = "dsh-v0.2.1-rc.2";
    std::string base;
    std::string target;
    std::string firstPatch;
    std::string secondPatch;
    std::string sourceHead;

    explicit UpstreamFixture(bool InConflict = false, const std::string& InIgnoredPath = {}) {
        std::filesystem::create_directories(repo);
        RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "initialize local-only fixture");
        for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
                 {"user.name", "Kano Test"}, {"user.email", "kano-test@example.invalid"},
                 {"commit.gpgsign", "false"}, {"core.autocrlf", "false"},
                 {"core.hooksPath", (sandbox.root / "empty-hooks").generic_string()}}) {
            RequireSuccess(RunGit({"config", key, value}, repo), "configure isolated fixture");
        }
        RequireSuccess(RunGit({"checkout", "-b", sourceBranch}, repo), "create maintenance branch");
        WriteFile(repo / ".gitignore", ".kano/\n" + InIgnoredPath);
        WriteFile(repo / "shared.txt", "base\n");
        RequireSuccess(RunGit({"add", ".gitignore", "shared.txt"}, repo), "stage base fixture");
        RequireSuccess(RunGit({"commit", "-m", "base release"}, repo), "commit base release");
        base = GitOutput(repo, {"rev-parse", "HEAD"});
        RequireSuccess(RunGit({"tag", oldTag}, repo), "tag old DSH prerelease");
        RequireSuccess(RunGit({"checkout", "-b", "upstream-fixture"}, repo), "create upstream lineage");
        target = Commit(InConflict ? "shared.txt" : "upstream.txt", "upstream\n", "new upstream release");
        RequireSuccess(RunGit({"tag", newTag}, repo), "tag new DSH prerelease");
        RequireSuccess(RunGit({"checkout", sourceBranch}, repo), "return to maintenance lineage");
        firstPatch = Commit("kano-one.txt", "first maintenance change\n", "first maintenance patch");
        secondPatch = Commit(InConflict ? "shared.txt" : "kano-two.txt", "second maintenance change\n", "second maintenance patch");
        sourceHead = secondPatch;
        WriteFile(repo / ".git" / "FETCH_HEAD", "unchanged offline fetch sentinel\n");
    }

    ~UpstreamFixture() { RemoveSandboxWorkspace(sandbox); }
    UpstreamFixture(const UpstreamFixture&) = delete;
    UpstreamFixture& operator=(const UpstreamFixture&) = delete;

    std::string Commit(const std::string& InPath, const std::string& InBytes, const std::string& InMessage) {
        WriteFile(repo / InPath, InBytes);
        RequireSuccess(RunGit({"add", "--", InPath}, repo), "stage fixture change");
        RequireSuccess(RunGit({"commit", "-m", InMessage}, repo), "commit fixture change");
        return GitOutput(repo, {"rev-parse", "HEAD"});
    }

    std::vector<std::string> Arguments() const {
        return {"sync", "upstream", "--repo", repo.string(), "--base", oldTag,
                "--ref", newTag, "--branch", freshBranch};
    }

    CommandResult Sync(std::vector<std::string> InArgs) const {
        return RunKogWithEnv(InArgs, sandbox.root,
                             {{"KOG_PROCESS_DIAGNOSTICS", "0"},
                              {"KOG_PROCESS_DIAGNOSTICS_LOG", (sandbox.root / "process-diag.log").string()},
                              {"KOG_SHELL_TIMEOUT_MS", "10000"}, {"GIT_OPTIONAL_LOCKS", "0"}});
    }

    std::filesystem::path PatchesFile(const std::string& InText) const {
        const auto path = sandbox.root / "selected-patches.txt";
        WriteFile(path, InText);
        return path;
    }

    void RequireSourcePreserved() const {
        REQUIRE(GitOutput(repo, {"rev-parse", "refs/heads/" + sourceBranch}) == sourceHead);
        REQUIRE(GitOutput(repo, {"rev-parse", oldTag}) == base);
        REQUIRE(GitOutput(repo, {"rev-parse", newTag}) == target);
        REQUIRE(ReadFile(repo / ".git" / "FETCH_HEAD") == "unchanged offline fetch sentinel\n");
        REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/stash"}, repo).exitCode != 0);
    }
};

void SetArgument(std::vector<std::string>& InArgs, const std::string& InKey, const std::string& InValue) {
    for (std::size_t index = 0; index + 1 < InArgs.size(); ++index) {
        if (InArgs[index] == InKey) { InArgs[index + 1] = InValue; return; }
    }
    FAIL("Missing fixture argument " << InKey);
}

} // namespace

TEST_CASE("sync_upstream_replays_dsh_prerelease_onto_fresh_branch_preserving_source",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    const auto result = fixture.Sync(fixture.Arguments());
    RequireSuccess(result, "replay explicit DSH prerelease maintenance patches");
    fixture.RequireSourcePreserved();
    REQUIRE(GitOutput(fixture.repo, {"symbolic-ref", "--short", "HEAD"}) == fixture.freshBranch);
    RequireSuccess(RunGit({"merge-base", "--is-ancestor", fixture.target, "HEAD"}, fixture.repo), "new release is target ancestor");
    REQUIRE(GitOutput(fixture.repo, {"rev-list", "--count", fixture.target + "..HEAD"}) == "2");
    REQUIRE(ReadFile(fixture.repo / "upstream.txt") == "upstream\n");
    REQUIRE(ReadFile(fixture.repo / "kano-one.txt") == "first maintenance change\n");
    REQUIRE(ReadFile(fixture.repo / "kano-two.txt") == "second maintenance change\n");
    Contains(GitOutput(fixture.repo, {"show", "-s", "--format=%B", "HEAD~1"}),
             "(cherry picked from commit " + fixture.firstPatch + ")");
    Contains(GitOutput(fixture.repo, {"show", "-s", "--format=%B", "HEAD"}),
             "(cherry picked from commit " + fixture.secondPatch + ")");
    const auto output = result.stdoutText + result.stderrText;
    for (const auto& value : {fixture.base, fixture.sourceHead, fixture.target,
                              fixture.freshBranch, fixture.firstPatch}) Contains(output, value);
    for (const auto* label : {"base", "head", "target", "branch", "retained", "dropped"}) Contains(output, label);
    REQUIRE(GitOutput(fixture.repo, {"status", "--porcelain=v1", "--untracked-files=all"}).empty());
}

TEST_CASE("sync_upstream_dry_run_preserves_repository_state",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    const auto selected = fixture.PatchesFile("# Keep one patch\n\n" + fixture.firstPatch + "\n");
    auto args = fixture.Arguments();
    args.insert(args.end(), {"--patches-file", selected.string(), "--dry-run"});
    const auto before = CaptureFiles(fixture.repo);
    const auto result = fixture.Sync(args);
    RequireSuccess(result, "preview without repository writes");
    RequireUnchanged(fixture.repo, before);
    Contains(result.stdoutText + result.stderrText, fixture.firstPatch);
    Contains(result.stdoutText + result.stderrText, fixture.secondPatch);
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + fixture.freshBranch}, fixture.repo).exitCode != 0);
}

TEST_CASE("sync_upstream_rejects_existing_target_and_dirty_worktree_without_mutation",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    SECTION("existing target branch") {
        RequireSuccess(RunGit({"branch", fixture.freshBranch, fixture.base}, fixture.repo), "create protected target branch");
    }
    SECTION("unstaged tracked change") { WriteFile(fixture.repo / "shared.txt", "unrelated dirty bytes\n"); }
    SECTION("staged change") {
        WriteFile(fixture.repo / "shared.txt", "unrelated staged bytes\n");
        RequireSuccess(RunGit({"add", "shared.txt"}, fixture.repo), "stage protected dirty bytes");
    }
    SECTION("untracked file") { WriteFile(fixture.repo / "untracked.txt", "unrelated untracked bytes\n"); }
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(fixture.Arguments()));
    RequireUnchanged(fixture.repo, before);
}

TEST_CASE("sync_upstream_rejects_bad_refs_base_and_nonlinear_history",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    auto args = fixture.Arguments();
    SECTION("missing target ref") { SetArgument(args, "--ref", "missing-release"); }
    SECTION("missing base ref") { SetArgument(args, "--base", "missing-base"); }
    SECTION("noncommit target") { SetArgument(args, "--ref", "HEAD^{tree}"); }
    SECTION("base outside source ancestry") { SetArgument(args, "--base", fixture.newTag); }
    SECTION("base outside target ancestry") { SetArgument(args, "--base", fixture.firstPatch); }
    SECTION("invalid fresh branch name") { SetArgument(args, "--branch", "invalid..branch"); }
    SECTION("merge in maintenance range") {
        RequireSuccess(RunGit({"checkout", "-b", "side", fixture.base}, fixture.repo), "create side lineage");
        fixture.Commit("side.txt", "side change\n", "side commit");
        RequireSuccess(RunGit({"checkout", fixture.sourceBranch}, fixture.repo), "return to source before merge");
        RequireSuccess(RunGit({"merge", "--no-ff", "-m", "merge side lineage", "side"}, fixture.repo), "create nonlinear maintenance history");
    }
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(args));
    RequireUnchanged(fixture.repo, before);
}

TEST_CASE("sync_upstream_rejects_invalid_duplicate_and_out_of_order_patch_lists",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    std::string patches;
    SECTION("invalid token") { patches = "not-a-commit\n"; }
    SECTION("abbreviated commit") { patches = fixture.firstPatch.substr(0, 12) + "\n"; }
    SECTION("unknown full commit") { patches = std::string(40, 'a') + "\n"; }
    SECTION("duplicate commit") { patches = fixture.firstPatch + "\n" + fixture.firstPatch + "\n"; }
    SECTION("commit outside range") { patches = fixture.target + "\n"; }
    SECTION("reversed order") { patches = fixture.secondPatch + "\n" + fixture.firstPatch + "\n"; }
    auto args = fixture.Arguments();
    args.insert(args.end(), {"--patches-file", fixture.PatchesFile(patches).string()});
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(args));
    RequireUnchanged(fixture.repo, before);
}

TEST_CASE("sync_upstream_replays_selected_patches_and_reports_dropped_commits",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    bool retainFirst = false;
    SECTION("reviewed subset with comments and blank lines") { retainFirst = true; }
    SECTION("empty file explicitly retires every local patch") {}
    const auto selected = fixture.PatchesFile(retainFirst ? "# reviewed subset\r\n\r\n" + fixture.firstPatch + "\r\n" : "");
    auto args = fixture.Arguments();
    args.insert(args.end(), {"--patches-file", selected.string()});
    const auto result = fixture.Sync(args);
    RequireSuccess(result, "apply reviewed patch-retirement selection");
    fixture.RequireSourcePreserved();
    REQUIRE(GitOutput(fixture.repo, {"rev-list", "--count", fixture.target + "..HEAD"}) == (retainFirst ? "1" : "0"));
    REQUIRE(std::filesystem::exists(fixture.repo / "kano-one.txt") == retainFirst);
    REQUIRE_FALSE(std::filesystem::exists(fixture.repo / "kano-two.txt"));
    const auto output = result.stdoutText + result.stderrText;
    Contains(output, "retained");
    Contains(output, "dropped");
    Contains(output, fixture.firstPatch);
    Contains(output, fixture.secondPatch);
    if (retainFirst) {
        Contains(GitOutput(fixture.repo, {"show", "-s", "--format=%B", "HEAD"}),
                 "(cherry picked from commit " + fixture.firstPatch + ")");
    } else {
        REQUIRE(GitOutput(fixture.repo, {"rev-parse", "HEAD"}) == fixture.target);
    }
}

TEST_CASE("sync_upstream_conflict_retains_recoverable_cherry_pick_state",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture(true);
    const auto result = fixture.Sync(fixture.Arguments());
    RequireFailure(result);
    fixture.RequireSourcePreserved();
    REQUIRE(GitOutput(fixture.repo, {"symbolic-ref", "--short", "HEAD"}) == fixture.freshBranch);
    REQUIRE(GitOutput(fixture.repo, {"rev-parse", "CHERRY_PICK_HEAD"}) == fixture.secondPatch);
    REQUIRE_FALSE(GitOutput(fixture.repo, {"ls-files", "--unmerged"}).empty());
    REQUIRE(GitOutput(fixture.repo, {"rev-list", "--count", fixture.target + "..HEAD"}) == "1");
    Contains(GitOutput(fixture.repo, {"show", "-s", "--format=%B", "HEAD"}),
             "(cherry picked from commit " + fixture.firstPatch + ")");
    const auto output = result.stdoutText + result.stderrText;
    Contains(output, "kog cherry-pick --continue");
    Contains(output, "kog cherry-pick --abort");
    Contains(output, "--repo");
    SECTION("caller can resolve and continue") {
        WriteFile(fixture.repo / "shared.txt", "resolved maintenance change\n");
        RequireSuccess(RunGit({"add", "shared.txt"}, fixture.repo), "stage explicit conflict resolution");
        RequireSuccess(RunGit({"-c", "core.editor=true", "cherry-pick", "--continue"}, fixture.repo), "continue retained cherry-pick");
        REQUIRE(GitOutput(fixture.repo, {"status", "--porcelain=v1"}).empty());
        REQUIRE(GitOutput(fixture.repo, {"rev-list", "--count", fixture.target + "..HEAD"}) == "2");
        Contains(GitOutput(fixture.repo, {"show", "-s", "--format=%B", "HEAD"}),
                 "(cherry picked from commit " + fixture.secondPatch + ")");
        fixture.RequireSourcePreserved();
    }
    SECTION("caller can abort the conflicted patch") {
        RequireSuccess(RunGit({"cherry-pick", "--abort"}, fixture.repo), "abort retained cherry-pick");
        REQUIRE(GitOutput(fixture.repo, {"status", "--porcelain=v1"}).empty());
        REQUIRE(GitOutput(fixture.repo, {"rev-list", "--count", fixture.target + "..HEAD"}) == "1");
        fixture.RequireSourcePreserved();
    }
}

TEST_CASE("sync_upstream_rejects_ambiguous_refs_without_mutation",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture;
    RequireSuccess(RunGit({"branch", "ambiguous-release", fixture.target}, fixture.repo), "create ambiguous branch");
    RequireSuccess(RunGit({"tag", "ambiguous-release", fixture.sourceHead}, fixture.repo), "create ambiguous tag");
    auto args = fixture.Arguments();
    SECTION("ambiguous target") { SetArgument(args, "--ref", "ambiguous-release"); }
    SECTION("ambiguous base") { SetArgument(args, "--base", "ambiguous-release"); }
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(args));
    RequireUnchanged(fixture.repo, before);
}

TEST_CASE("sync_upstream_preserves_ignored_files_that_upstream_would_track",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture(false, "payload.bin\n");
    RequireSuccess(RunGit({"checkout", "upstream-fixture"}, fixture.repo), "prepare upstream tracked payload");
    WriteFile(fixture.repo / "payload.bin", "upstream tracked payload\n");
    RequireSuccess(RunGit({"add", "-f", "--", "payload.bin"}, fixture.repo), "track ignored upstream payload");
    RequireSuccess(RunGit({"commit", "-m", "upstream starts tracking payload"}, fixture.repo), "commit upstream payload");
    RequireSuccess(RunGit({"tag", "dsh-v0.2.1-rc.3"}, fixture.repo), "tag upstream payload release");
    RequireSuccess(RunGit({"checkout", fixture.sourceBranch}, fixture.repo), "restore maintenance source branch");
    WriteFile(fixture.repo / "payload.bin", "user ignored bytes must survive\n");
    REQUIRE(GitOutput(fixture.repo, {"status", "--porcelain=v1", "--untracked-files=all"}).empty());
    RequireSuccess(RunGit({"check-ignore", "--quiet", "payload.bin"}, fixture.repo), "confirm user payload is ignored");
    auto args = fixture.Arguments();
    SetArgument(args, "--ref", "dsh-v0.2.1-rc.3");
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(args));
    RequireUnchanged(fixture.repo, before);
    REQUIRE(ReadFile(fixture.repo / "payload.bin") == "user ignored bytes must survive\n");
    REQUIRE(GitOutput(fixture.repo, {"symbolic-ref", "--short", "HEAD"}) == fixture.sourceBranch);
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + fixture.freshBranch}, fixture.repo).exitCode != 0);
    fixture.RequireSourcePreserved();
}

TEST_CASE("sync_upstream_preserves_ignored_files_that_retained_patches_would_track",
          "[functional][sync][upstream][KOG-TSK-0145]") {
    UpstreamFixture fixture(false, "payload.bin\n");
    WriteFile(fixture.repo / "payload.bin", "maintenance tracked payload\n");
    RequireSuccess(RunGit({"add", "-f", "--", "payload.bin"}, fixture.repo), "track ignored maintenance payload");
    RequireSuccess(RunGit({"commit", "-m", "maintenance starts tracking payload"}, fixture.repo), "commit retained payload addition");
    const auto addPatch = GitOutput(fixture.repo, {"rev-parse", "HEAD"});
    RequireSuccess(RunGit({"rm", "--", "payload.bin"}, fixture.repo), "remove payload in later source history");
    RequireSuccess(RunGit({"commit", "-m", "maintenance stops tracking payload"}, fixture.repo), "commit later payload deletion");
    fixture.sourceHead = GitOutput(fixture.repo, {"rev-parse", "HEAD"});
    bool retainDeletion = false;
    SECTION("retain only the path addition") {}
    SECTION("retain the addition and later deletion") { retainDeletion = true; }
    WriteFile(fixture.repo / "payload.bin", "user ignored bytes must survive\n");
    REQUIRE(GitOutput(fixture.repo, {"status", "--porcelain=v1", "--untracked-files=all"}).empty());
    RequireSuccess(RunGit({"check-ignore", "--quiet", "payload.bin"}, fixture.repo), "confirm user payload is ignored");
    auto args = fixture.Arguments();
    const auto patches = fixture.PatchesFile(addPatch + "\n" + (retainDeletion ? fixture.sourceHead + "\n" : ""));
    args.insert(args.end(), {"--patches-file", patches.string()});
    const auto before = CaptureFiles(fixture.repo);
    RequireFailure(fixture.Sync(args));
    RequireUnchanged(fixture.repo, before);
    REQUIRE(ReadFile(fixture.repo / "payload.bin") == "user ignored bytes must survive\n");
    REQUIRE(GitOutput(fixture.repo, {"symbolic-ref", "--short", "HEAD"}) == fixture.sourceBranch);
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + fixture.freshBranch}, fixture.repo).exitCode != 0);
    fixture.RequireSourcePreserved();
}

} // namespace kano::git::tests::functional
