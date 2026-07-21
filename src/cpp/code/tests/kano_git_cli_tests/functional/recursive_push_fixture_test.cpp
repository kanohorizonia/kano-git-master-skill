#include "functional_test_support.hpp"

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

auto StripProcessDiagnostics(const std::string& InText) -> std::string {
    std::istringstream input(InText);
    std::ostringstream output;
    std::string line;
    bool first = true;
    while (std::getline(input, line)) {
        if (line.find("[process-diag]") != std::string::npos) {
            continue;
        }
        if (!first) {
            output << '\n';
        }
        output << line;
        first = false;
    }
    return output.str();
}

auto CurrentHeadSha(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"rev-parse", "HEAD"}, InRepo);
    RequireSuccess(result, "rev-parse HEAD");
    return TrimCopy(result.stdoutText);
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"symbolic-ref", "--quiet", "--short", "HEAD"}, InRepo);
    RequireSuccess(result, "current branch");
    return TrimCopy(result.stdoutText);
}

auto StatusPorcelain(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"status", "--porcelain"}, InRepo);
    RequireSuccess(result, "git status --porcelain");
    return TrimCopy(result.stdoutText);
}

auto IndexStatus(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"diff", "--cached", "--name-status"}, InRepo);
    RequireSuccess(result, "git diff --cached --name-status");
    return TrimCopy(result.stdoutText);
}

auto RefSha(const std::filesystem::path& InRepo, const std::string& InRef) -> std::string {
    const auto result = RunGit({"rev-parse", InRef}, InRepo);
    RequireSuccess(result, "rev-parse ref");
    return TrimCopy(result.stdoutText);
}

auto RefExists(const std::filesystem::path& InRepo, const std::string& InRef) -> bool {
    return RunGit({"show-ref", "--verify", "--quiet", InRef}, InRepo).exitCode == 0;
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

auto InitRootWithoutRemote(const SandboxContext& InSandbox, const std::string& InName = "root") -> std::filesystem::path {
    const auto root = (InSandbox.root / InName).lexically_normal();
    std::filesystem::create_directories(root);
    RequireSuccess(RunGit({"init", root.string()}, InSandbox.root), "init root without remote");
    ConfigureIdentity(root);
    WriteTextFile(root / ".gitignore", ".kano/\n");
    WriteTextFile(root / "README.md", "root\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, root), "root add");
    RequireSuccess(RunGit({"commit", "-m", "seed root"}, root), "root commit");
    return root;
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

auto RunDiscoverFull(const std::filesystem::path& InRoot, const int InDepth = 5) -> std::string {
    const auto result = RunKog({"discover", "--full", "--unregistered-depth", std::to_string(InDepth), "--format", "json", "--repo-root", InRoot.string(), "--no-cache"}, InRoot);
    RequireSuccess(result, "kog discover full");
    return StripAnsi(result.stdoutText);
}

auto RunPushRecursive(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"push", "--recursive", "--bottom-up", "--jobs", "1"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto RunPushCurrentOnly(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"push", "--no-recursive"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto RunConvergeDryRun(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"converge", "--dry-run"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto PositionOf(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    const auto pos = InText.find(InNeedle);
    INFO("needle=" << InNeedle);
    INFO(InText);
    REQUIRE(pos != std::string::npos);
    return pos;
}

auto ProcessingLinePositionOf(const std::string& InText, const std::filesystem::path& InRepo) -> std::size_t {
    const auto expected = InRepo.lexically_normal().generic_string();
    std::size_t offset = 0;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto marker = line.find("] Processing ");
        if (marker != std::string::npos) {
            const auto actual = line.substr(marker + std::string("] Processing ").size());
            if (actual == expected) {
                return offset + marker;
            }
        }
        offset += line.size() + 1;
    }
    INFO("missing processing repo=" << expected);
    INFO(InText);
    REQUIRE(false);
    return std::string::npos;
}

} // namespace

TEST_CASE("recursive_push_root_no_remote_skips_container_and_pushes_child", "[functional][push][recursive][root-no-remote]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-root-no-remote");
    const auto root = InitRootWithoutRemote(sandbox);
    auto child = CreateRemote(sandbox, "child");
    const auto childPath = AddSubmodule(root, child, "deps/child", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n\tkog-hygiene = true\n");

    const auto expectedChildHead = CommitFile(childPath, "child.txt", "child recursive push\n", "child recursive push");
    const auto beforeRootHead = CurrentHeadSha(root);

    const auto result = RunPushRecursive(root);
    const auto output = StripProcessDiagnostics(StripAnsi(result.stdoutText + "\n" + result.stderrText));
    RequireSuccess(result, "recursive push root no remote");
    RequireContains(output, "SKIPPED_NO_REMOTE");
    RequireContains(output, "skipped_no_remote=1");
    RequireContains(output, "Pushed (origin, main)");
    REQUIRE(RefSha(child.bare, "refs/heads/main") == expectedChildHead);
    REQUIRE(CurrentHeadSha(root) == beforeRootHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_nested_build_base_before_parent_and_distinct_products", "[functional][push][recursive][order][Build/Base]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-build-base-order");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto productABase = CreateRemote(sandbox, "product-a-base");
    auto productBBase = CreateRemote(sandbox, "product-b-base");

    const auto root = rootRemote.clone;
    const auto aBasePath = AddSubmodule(root, productABase, "ProductA/Build/Base", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    const auto bBasePath = AddSubmodule(root, productBBase, "ProductB/Build/Base", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push submodule registration");

    const auto aHead = CommitFile(aBasePath, "base-a.txt", "A base pushed\n", "push product a base");
    const auto bHead = CommitFile(bBasePath, "base-b.txt", "B base pushed\n", "push product b base");
    RequireSuccess(RunGit({"add", "ProductA/Build/Base", "ProductB/Build/Base"}, root), "stage root gitlinks");
    const auto rootHead = CommitFile(root, "root.txt", "root references bases\n", "push root pointers");

    const auto result = RunPushRecursive(root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive push build base order");
    REQUIRE(ProcessingLinePositionOf(output, aBasePath) < ProcessingLinePositionOf(output, root));
    REQUIRE(ProcessingLinePositionOf(output, bBasePath) < ProcessingLinePositionOf(output, root));
    REQUIRE(RefSha(productABase.bare, "refs/heads/main") == aHead);
    REQUIRE(RefSha(productBBase.bare, "refs/heads/main") == bHead);
    REQUIRE(RefSha(rootRemote.bare, "refs/heads/main") == rootHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_nested_repo_without_remote_fails_and_blocks_parent", "[functional][push][recursive][child-failure][blocked-parent]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-child-failure");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/child", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"remote", "remove", "origin"}, childPath), "remove child origin");
    const auto childHead = CommitFile(childPath, "child.txt", "unpushed child without remote\n", "child cannot push");
    RequireSuccess(RunGit({"add", "deps/child"}, root), "stage unpushed child gitlink");
    CommitFile(root, "root.txt", "parent references failing child\n", "parent references child");
    const auto beforeRootRemote = RefSha(rootRemote.bare, "refs/heads/main");
    const auto beforeChildRemote = RefSha(childRemote.bare, "refs/heads/main");

    const auto result = RunPushRecursive(root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "recursive push child failure");
    RequireContains(output, "FAILED_MISSING_REMOTE");
    RequireContains(output, "BLOCKED_BY_CHILD_FAILURE");
    REQUIRE(RefSha(rootRemote.bare, "refs/heads/main") == beforeRootRemote);
    REQUIRE(RefSha(childRemote.bare, "refs/heads/main") == beforeChildRemote);
    REQUIRE(CurrentHeadSha(childPath) == childHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_dry_run_preserves_refs_status_head_branch_and_converge_state", "[functional][push][recursive][dry-run][no-mutation]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-dry-run");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/child", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push registration before dry-run");
    CommitFile(childPath, "child.txt", "dry run child\n", "dry run child");
    RequireSuccess(RunGit({"add", "deps/child"}, root), "stage dry-run gitlink");
    CommitFile(root, "root.txt", "dry run root\n", "dry run root");

    const auto beforeRootStatus = StatusPorcelain(root);
    const auto beforeChildStatus = StatusPorcelain(childPath);
    const auto beforeRootIndex = IndexStatus(root);
    const auto beforeChildIndex = IndexStatus(childPath);
    const auto beforeRootHead = CurrentHeadSha(root);
    const auto beforeChildHead = CurrentHeadSha(childPath);
    const auto beforeRootBranch = CurrentBranch(root);
    const auto beforeChildBranch = CurrentBranch(childPath);
    const auto beforeRootRemote = RefSha(rootRemote.bare, "refs/heads/main");
    const auto beforeChildRemote = RefSha(childRemote.bare, "refs/heads/main");
    const auto statePath = (root / ".kano" / "workspace-state.json").lexically_normal();
    const bool beforeStateExists = std::filesystem::exists(statePath);

    const auto result = RunPushRecursive(root, {"--dry-run"});
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive push dry-run");
    RequireContains(output, "[DRY RUN]");
    RequireContains(output, "Would run: git push origin main");
    REQUIRE(StatusPorcelain(root) == beforeRootStatus);
    REQUIRE(StatusPorcelain(childPath) == beforeChildStatus);
    REQUIRE(IndexStatus(root) == beforeRootIndex);
    REQUIRE(IndexStatus(childPath) == beforeChildIndex);
    REQUIRE(CurrentHeadSha(root) == beforeRootHead);
    REQUIRE(CurrentHeadSha(childPath) == beforeChildHead);
    REQUIRE(CurrentBranch(root) == beforeRootBranch);
    REQUIRE(CurrentBranch(childPath) == beforeChildBranch);
    REQUIRE(RefSha(rootRemote.bare, "refs/heads/main") == beforeRootRemote);
    REQUIRE(RefSha(childRemote.bare, "refs/heads/main") == beforeChildRemote);
    REQUIRE(std::filesystem::exists(statePath) == beforeStateExists);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_trusted_unregistered_top_level_child_and_ignored_nested_behaviour", "[functional][push][recursive][trusted-unregistered][ignored]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-trusted-unregistered");
    const auto root = InitRootWithoutRemote(sandbox);
    auto topChild = CreateRemote(sandbox, "top-child");
    const auto topChildPath = (root / "tools" / "top-child").lexically_normal();
    std::filesystem::create_directories(topChildPath.parent_path());
    RequireSuccess(RunGit({"clone", topChild.bare.string(), topChildPath.string()}, sandbox.root), "clone top-level unregistered child");
    ConfigureIdentity(topChildPath);
    const auto ignoredPath = (root / "build" / "ignored-child").lexically_normal();
    std::filesystem::create_directories(ignoredPath.parent_path());
    RequireSuccess(RunGit({"clone", topChild.bare.string(), ignoredPath.string()}, sandbox.root), "clone ignored nested child");
    ConfigureIdentity(ignoredPath);

    const auto discover = RunDiscoverFull(root, 4);
    RequireContains(discover, "top-child");
    RequireContains(discover, "\"type\":\"unregistered\"");
    RequireNotContains(discover, "ignored-child");
    const auto expectedTopChildHead = CommitFile(topChildPath, "trusted.txt", "trusted push\n", "trusted unregistered push");

    const auto result = RunPushRecursive(root);
    const auto output = StripProcessDiagnostics(StripAnsi(result.stdoutText + "\n" + result.stderrText));
    RequireSuccess(result, "trusted unregistered recursive push");
    RequireContains(output, "top-child");
    RequireNotContains(output, "ignored-child");
    RequireContains(output, "Push skipped: no pushable remote on workspace root container repo");
    REQUIRE(RefSha(topChild.bare, "refs/heads/main") == expectedTopChildHead);

    InitRootWithoutRemote(sandbox, "untrusted-root");
    const auto untrustedRoot = (sandbox.root / "untrusted-root").lexically_normal();
    const auto untrustedNested = (untrustedRoot / "nested" / "new-child").lexically_normal();
    std::filesystem::create_directories(untrustedNested.parent_path());
    RequireSuccess(RunGit({"clone", topChild.bare.string(), untrustedNested.string()}, sandbox.root), "clone newly discovered untrusted child");
    const auto converge = RunConvergeDryRun(untrustedRoot, {"--unregistered-scan"});
    const auto convergeOutput = StripAnsi(converge.stdoutText + "\n" + converge.stderrText);
    RequireFailure(converge, "untrusted nested converge block");
    RequireContains(convergeOutput, "Discovered unregistered nested Git repository that is not in the trusted workspace manifest");
    RequireContains(convergeOutput, "Blocked repos");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_skips_repos_with_kog_push_false", "[functional][push][recursive][kog-push-false]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-kog-push-false");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/no-push", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push kog-push false registration");
    const auto beforeChildRemote = RefSha(childRemote.bare, "refs/heads/main");
    const auto childHead = CommitFile(childPath, "child.txt", "should not push\n", "child push disabled");

    const auto discover = RunDiscoverFull(root, 3);
    RequireContains(discover, "no-push");
    RequireContains(discover, "\"type\":\"registered\"");
    RequireContains(discover, "\"kogPush\":\"false\"");
    const auto result = RunPushRecursive(root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireSuccess(result, "recursive push kog-push=false");
    RequireContains(output, "no-push");
    RequireContains(output, "SKIPPED_BY_POLICY");
    RequireContains(output, "commandPolicy.push=false / kog-push=false");
    REQUIRE(RefSha(childRemote.bare, "refs/heads/main") == beforeChildRemote);
    REQUIRE(CurrentHeadSha(childPath) == childHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_actual_dirty_nested_repo_without_remote_fails_missing_remote", "[functional][push][recursive][missing-remote]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-dirty-missing-remote");
    const auto root = InitRootWithoutRemote(sandbox);
    auto child = CreateRemote(sandbox, "dirty-child");
    const auto nested = AddSubmodule(root, child, "tools/dirty-child", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"remote", "remove", "origin"}, nested), "remove dirty nested origin");
    WriteTextFile(nested / "dirty.txt", "actual dirty work\n");

    const auto result = RunPushRecursive(root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "recursive push dirty missing remote");
    RequireContains(output, "dirty-child");
    RequireContains(output, "FAILED_MISSING_REMOTE");
    RequireContains(output, "no usable push remote found");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_push_disabled_child_unavailable_pointer_blocks_parent", "[functional][push][recursive][policy-block]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-disabled-pointer-block");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto childRemote = CreateRemote(sandbox, "child");
    const auto root = rootRemote.clone;
    const auto childPath = AddSubmodule(root, childRemote, "deps/no-push", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push policy registration");
    const auto beforeRootRemote = RefSha(rootRemote.bare, "refs/heads/main");
    const auto beforeChildRemote = RefSha(childRemote.bare, "refs/heads/main");
    const auto childHead = CommitFile(childPath, "child.txt", "unavailable policy child\n", "policy child unavailable");
    RequireSuccess(RunGit({"add", "deps/no-push"}, root), "stage no-push gitlink");
    CommitFile(root, "root.txt", "parent references push-disabled child\n", "parent references push-disabled child");

    const auto result = RunPushRecursive(root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "push-disabled child unavailable pointer blocks parent");
    RequireContains(output, "SKIPPED_BY_POLICY");
    RequireContains(output, "BLOCKED_BY_CHILD_FAILURE");
    RequireContains(output, "push-disabled child");
    REQUIRE(RefSha(rootRemote.bare, "refs/heads/main") == beforeRootRemote);
    REQUIRE(RefSha(childRemote.bare, "refs/heads/main") == beforeChildRemote);
    REQUIRE(CurrentHeadSha(childPath) == childHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_child_failure_does_not_stop_unrelated_sibling_push", "[functional][push][recursive][best-effort]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-best-effort-siblings");
    auto rootRemote = CreateRemote(sandbox, "root");
    auto failingRemote = CreateRemote(sandbox, "failing-child");
    auto healthyRemote = CreateRemote(sandbox, "healthy-child");
    const auto root = rootRemote.clone;
    const auto failingPath = AddSubmodule(root, failingRemote, "deps/failing", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    const auto healthyPath = AddSubmodule(root, healthyRemote, "deps/healthy", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = true\n");
    RequireSuccess(RunGit({"push", "origin", rootRemote.branch}, root), "push sibling registration");
    RequireSuccess(RunGit({"remote", "remove", "origin"}, failingPath), "remove failing child origin");
    const auto failingHead = CommitFile(failingPath, "failing.txt", "cannot push\n", "failing child cannot push");
    const auto healthyHead = CommitFile(healthyPath, "healthy.txt", "can push\n", "healthy child can push");
    RequireSuccess(RunGit({"add", "deps/failing", "deps/healthy"}, root), "stage sibling gitlinks");
    CommitFile(root, "root.txt", "parent references siblings\n", "parent references siblings");
    const auto beforeRootRemote = RefSha(rootRemote.bare, "refs/heads/main");
    const auto beforeFailingRemote = RefSha(failingRemote.bare, "refs/heads/main");

    const auto result = RunKog({"push", "--recursive", "--bottom-up", "--jobs", "2"}, root);
    const auto output = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    RequireFailure(result, "one child failure does not stop unrelated sibling push");
    RequireContains(output, "FAILED_MISSING_REMOTE");
    RequireContains(output, "Pushed (origin, main)");
    RequireContains(output, "BLOCKED_BY_CHILD_FAILURE");
    REQUIRE(RefSha(healthyRemote.bare, "refs/heads/main") == healthyHead);
    REQUIRE(RefSha(failingRemote.bare, "refs/heads/main") == beforeFailingRemote);
    REQUIRE(RefSha(rootRemote.bare, "refs/heads/main") == beforeRootRemote);
    REQUIRE(CurrentHeadSha(failingPath) == failingHead);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("recursive_push_explicit_edge_outcomes_for_detached_no_commits_and_missing_remote_branch", "[functional][push][recursive][edge-outcomes]") {
    const auto sandbox = CreateSandboxWorkspace("recursive-push-edge-outcomes");

    auto detachedRemote = CreateRemote(sandbox, "detached");
    const auto detachedRoot = detachedRemote.clone;
    const auto detachedHead = CurrentHeadSha(detachedRoot);
    RequireSuccess(RunGit({"checkout", "--detach", detachedHead}, detachedRoot), "detach root head");
    const auto detachedResult = RunPushCurrentOnly(detachedRoot);
    const auto detachedOutput = StripAnsi(detachedResult.stdoutText + "\n" + detachedResult.stderrText);
    RequireFailure(detachedResult, "detached HEAD push outcome");
    RequireContains(detachedOutput, "FAILED_PUSH");
    RequireContains(detachedOutput, "detached HEAD is not supported");

    const auto emptyRoot = (sandbox.root / "empty-root").lexically_normal();
    RequireSuccess(RunGit({"init", emptyRoot.string()}, sandbox.root), "init empty root");
    const auto emptyResult = RunPushCurrentOnly(emptyRoot);
    const auto emptyOutput = StripAnsi(emptyResult.stdoutText + "\n" + emptyResult.stderrText);
    RequireFailure(emptyResult, "no commits push outcome");
    RequireContains(emptyOutput, "FAILED_PUSH");
    RequireContains(emptyOutput, "repository has no commits to push");

    auto missingBranchRemote = CreateRemote(sandbox, "missing-branch");
    const auto missingBranchRoot = missingBranchRemote.clone;
    RequireSuccess(RunGit({"checkout", "-b", "feature/missing-remote-branch"}, missingBranchRoot), "checkout missing remote branch");
    const auto featureHead = CommitFile(missingBranchRoot, "feature.txt", "remote branch missing\n", "create missing remote branch");
    const auto missingBranchResult = RunPushCurrentOnly(missingBranchRoot);
    const auto missingBranchOutput = StripAnsi(missingBranchResult.stdoutText + "\n" + missingBranchResult.stderrText);
    RequireSuccess(missingBranchResult, "missing remote branch explicit push outcome");
    RequireContains(missingBranchOutput, "Pushed (origin, feature/missing-remote-branch)");
    REQUIRE(RefExists(missingBranchRemote.bare, "refs/heads/feature/missing-remote-branch"));
    REQUIRE(RefSha(missingBranchRemote.bare, "refs/heads/feature/missing-remote-branch") == featureHead);

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
