#include "bdd_scenario_recorder.hpp"
#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

struct RemoteCloneContext {
    SandboxContext sandbox;
    std::filesystem::path bareRemote;
    std::filesystem::path seedRepo;
    std::filesystem::path cloneRepo;
    std::string branch;
};

struct SubmoduleWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path childBareRemote;
    std::filesystem::path childSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::filesystem::path cloneChildRepo;
    std::string branch;
    std::string submodulePath;
};

struct NestedSubmoduleWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path leafBareRemote;
    std::filesystem::path leafSeedRepo;
    std::filesystem::path midBareRemote;
    std::filesystem::path midSeedRepo;
    std::filesystem::path rootBareRemote;
    std::filesystem::path rootSeedRepo;
    std::filesystem::path cloneRootRepo;
    std::filesystem::path cloneMidRepo;
    std::filesystem::path cloneLeafRepo;
    std::string branch;
    std::string midSubmodulePath;
    std::string leafSubmodulePath;
};

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
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

auto CountOccurrences(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    if (InNeedle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = InText.find(InNeedle, pos)) != std::string::npos) {
        ++count;
        pos += InNeedle.size();
    }
    return count;
}

auto TrimCopy(std::string InValue) -> std::string {
    const auto first = InValue.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(first, last - first + 1);
}

auto ScenarioMetadataPath(const std::string& InScenarioId) -> std::filesystem::path {
    if (const char* metadataDir = std::getenv("KANO_BDD_METADATA_DIR"); metadataDir != nullptr && metadataDir[0] != '\0') {
        return (std::filesystem::path(metadataDir) / (InScenarioId + ".json")).lexically_normal();
    }
    return (std::filesystem::current_path() / ".kano" / "tmp" / "test-metadata" / "bdd" / (InScenarioId + ".json")).lexically_normal();
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

auto RequireScenarioMetadata(const std::string& InScenarioId,
                             const std::string& InFeature,
                             const std::string& InDiagramType) -> std::filesystem::path {
    const auto sidecar = ScenarioMetadataPath(InScenarioId);
    REQUIRE(std::filesystem::exists(sidecar));
    const auto json = ReadTextFile(sidecar);
    RequireContains(json, "\"style\": \"bdd\"");
    RequireContains(json, "\"feature\": \"" + InFeature + "\"");
    RequireContains(json, "\"scenarioId\": \"" + InScenarioId + "\"");
    RequireContains(json, "\"featured\": true");
    RequireContains(json, "\"diagramType\": \"" + InDiagramType + "\"");
    RequireContains(json, "\"bdd\"");
    RequireContains(json, "\"feature:" + InFeature + "\"");
    RequireContains(json, "\"scenario:" + InScenarioId + "\"");
    RequireContains(json, "\"featured\"");
    return sidecar;
}

auto ConvergeStatePath(const std::filesystem::path& InRoot) -> std::filesystem::path {
    return (InRoot / ".kano" / "tmp" / "workflows" / "converge" / "state.json").lexically_normal();
}

auto FunctionalProcessDiagLogPath(const std::filesystem::path& InRoot) -> std::filesystem::path {
    return (InRoot / ".kano" / "tmp" / "functional-process-diag.log").lexically_normal();
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto GitStatusShort(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"status", "--short"}, InRepo);
    RequireSuccess(result, "git status --short");
    return result.stdoutText;
}

auto InitPlainGitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init plain git repo");
    ConfigureIdentity(InRepo);
    WriteTextFile(InRepo / "README.md", "repo\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add plain readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit plain readme");
}

auto CreateRemoteWithClone(const std::string& InName, const std::string& InBranch = "main") -> RemoteCloneContext {
    RemoteCloneContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.bareRemote = (ctx.sandbox.root / "remote.git").lexically_normal();
    ctx.seedRepo = (ctx.sandbox.root / "seed").lexically_normal();
    ctx.cloneRepo = (ctx.sandbox.root / "clone").lexically_normal();
    ctx.branch = InBranch;

    RequireSuccess(RunGit({"init", "--bare", ctx.bareRemote.string()}, ctx.sandbox.root), "init bare remote");
    RequireSuccess(RunGit({"init", ctx.seedRepo.string()}, ctx.sandbox.root), "init seed repo");
    ConfigureIdentity(ctx.seedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.seedRepo), "checkout seed branch");
    WriteTextFile(ctx.seedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.seedRepo), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed commit"}, ctx.seedRepo), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo), "seed add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.bareRemote), "set bare HEAD");
    RequireSuccess(RunGit({"clone", ctx.bareRemote.string(), ctx.cloneRepo.string()}, ctx.sandbox.root), "clone repo");
    ConfigureIdentity(ctx.cloneRepo);
    return ctx;
}

auto CreateRemoteWithSubmoduleClone(const std::string& InName, const std::string& InBranch = "main") -> SubmoduleWorkspaceContext {
    SubmoduleWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.childBareRemote = (ctx.sandbox.root / "child-remote.git").lexically_normal();
    ctx.childSeedRepo = (ctx.sandbox.root / "child-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.branch = InBranch;
    ctx.submodulePath = "deps/child";

    RequireSuccess(RunGit({"init", "--bare", ctx.childBareRemote.string()}, ctx.sandbox.root), "init child bare");
    RequireSuccess(RunGit({"init", ctx.childSeedRepo.string()}, ctx.sandbox.root), "init child seed");
    ConfigureIdentity(ctx.childSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.childSeedRepo), "checkout child branch");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "child seed\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.childSeedRepo), "child add");
    RequireSuccess(RunGit({"commit", "-m", "child seed"}, ctx.childSeedRepo), "child commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.childBareRemote.string()}, ctx.childSeedRepo), "child add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.childSeedRepo), "child push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.childBareRemote), "child bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root base commit");
    RequireSuccess(RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.childBareRemote.string(), ctx.submodulePath}, ctx.rootSeedRepo), "root add submodule");
    RequireSuccess(RunGit({"commit", "-am", "add submodule"}, ctx.rootSeedRepo), "root commit submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(RunGit({"-c", "protocol.file.allow=always", "clone", "--recurse-submodules", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()}, ctx.sandbox.root), "clone root");
    ConfigureIdentity(ctx.cloneRootRepo);
    ctx.cloneChildRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    ConfigureIdentity(ctx.cloneChildRepo);
    return ctx;
}

auto CreateRemoteWithNestedSubmoduleClone(const std::string& InName, const std::string& InBranch = "main") -> NestedSubmoduleWorkspaceContext {
    NestedSubmoduleWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.leafBareRemote = (ctx.sandbox.root / "leaf-remote.git").lexically_normal();
    ctx.leafSeedRepo = (ctx.sandbox.root / "leaf-seed").lexically_normal();
    ctx.midBareRemote = (ctx.sandbox.root / "mid-remote.git").lexically_normal();
    ctx.midSeedRepo = (ctx.sandbox.root / "mid-seed").lexically_normal();
    ctx.rootBareRemote = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    ctx.rootSeedRepo = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.cloneRootRepo = (ctx.sandbox.root / "root-clone").lexically_normal();
    ctx.branch = InBranch;
    ctx.midSubmodulePath = "deps/mid";
    ctx.leafSubmodulePath = "deps/leaf";

    RequireSuccess(RunGit({"init", "--bare", ctx.leafBareRemote.string()}, ctx.sandbox.root), "init leaf bare");
    RequireSuccess(RunGit({"init", ctx.leafSeedRepo.string()}, ctx.sandbox.root), "init leaf seed");
    ConfigureIdentity(ctx.leafSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.leafSeedRepo), "checkout leaf branch");
    WriteTextFile(ctx.leafSeedRepo / "leaf.txt", "leaf seed\n");
    RequireSuccess(RunGit({"add", "leaf.txt"}, ctx.leafSeedRepo), "leaf add");
    RequireSuccess(RunGit({"commit", "-m", "leaf seed"}, ctx.leafSeedRepo), "leaf commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.leafBareRemote.string()}, ctx.leafSeedRepo), "leaf add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.leafSeedRepo), "leaf push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.leafBareRemote), "leaf bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.midBareRemote.string()}, ctx.sandbox.root), "init mid bare");
    RequireSuccess(RunGit({"init", ctx.midSeedRepo.string()}, ctx.sandbox.root), "init mid seed");
    ConfigureIdentity(ctx.midSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.midSeedRepo), "checkout mid branch");
    WriteTextFile(ctx.midSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.midSeedRepo / "mid.txt", "mid seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "mid.txt"}, ctx.midSeedRepo), "mid add base");
    RequireSuccess(RunGit({"commit", "-m", "mid seed"}, ctx.midSeedRepo), "mid base commit");
    RequireSuccess(RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.leafBareRemote.string(), ctx.leafSubmodulePath}, ctx.midSeedRepo), "mid add leaf submodule");
    RequireSuccess(RunGit({"commit", "-am", "add leaf submodule"}, ctx.midSeedRepo), "mid commit leaf submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.midBareRemote.string()}, ctx.midSeedRepo), "mid add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.midSeedRepo), "mid push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.midBareRemote), "mid bare HEAD");

    RequireSuccess(RunGit({"init", "--bare", ctx.rootBareRemote.string()}, ctx.sandbox.root), "init root bare");
    RequireSuccess(RunGit({"init", ctx.rootSeedRepo.string()}, ctx.sandbox.root), "init root seed");
    ConfigureIdentity(ctx.rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.rootSeedRepo), "checkout root branch");
    WriteTextFile(ctx.rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(ctx.rootSeedRepo / "README.md", "root seed\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, ctx.rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, ctx.rootSeedRepo), "root base commit");
    RequireSuccess(RunGit({"-c", "protocol.file.allow=always", "submodule", "add", "-b", ctx.branch, ctx.midBareRemote.string(), ctx.midSubmodulePath}, ctx.rootSeedRepo), "root add mid submodule");
    RequireSuccess(RunGit({"commit", "-am", "add mid submodule"}, ctx.rootSeedRepo), "root commit mid submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.rootBareRemote.string()}, ctx.rootSeedRepo), "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.rootBareRemote), "root bare HEAD");

    RequireSuccess(RunGit({"-c", "protocol.file.allow=always", "clone", "--recurse-submodules", ctx.rootBareRemote.string(), ctx.cloneRootRepo.string()}, ctx.sandbox.root), "clone nested root");
    ConfigureIdentity(ctx.cloneRootRepo);
    ctx.cloneMidRepo = (ctx.cloneRootRepo / std::filesystem::path(ctx.midSubmodulePath)).lexically_normal();
    ctx.cloneLeafRepo = (ctx.cloneMidRepo / std::filesystem::path(ctx.leafSubmodulePath)).lexically_normal();
    ConfigureIdentity(ctx.cloneMidRepo);
    ConfigureIdentity(ctx.cloneLeafRepo);
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneMidRepo), "checkout mid clone branch");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneLeafRepo), "checkout leaf clone branch");
    return ctx;
}

auto RunConvergeDryRun(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"converge", "--dry-run"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto CommitAndPushFile(const std::filesystem::path& InRepo,
                       const std::string& InBranch,
                       const std::string& InRelativePath,
                       const std::string& InContent,
                       const std::string& InMessage) -> void {
    RequireSuccess(RunGit({"checkout", InBranch}, InRepo), "checkout branch for remote advance");
    WriteTextFile(InRepo / std::filesystem::path(InRelativePath), InContent);
    RequireSuccess(RunGit({"add", InRelativePath}, InRepo), "add remote advance file");
    RequireSuccess(RunGit({"commit", "-m", InMessage}, InRepo), "commit remote advance");
    RequireSuccess(RunGit({"push", "origin", InBranch}, InRepo), "push remote advance");
}

auto PlanPayload(const std::string& InText) -> std::string {
    const auto start = InText.find("Converge Plan");
    REQUIRE(start != std::string::npos);
    return InText.substr(start);
}

auto RequireOrderedAfter(const std::string& InText,
                         const std::string& InMarker,
                         const std::string& InFirst,
                         const std::string& InSecond) -> void {
    const auto marker = InText.find(InMarker);
    REQUIRE(marker != std::string::npos);
    const auto first = InText.find(InFirst, marker);
    const auto second = InText.find(InSecond, marker);
    INFO(InText);
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    REQUIRE(first < second);
}

auto RunDiscoverFull(const std::filesystem::path& InRoot, int InDepth = 3) -> void {
    const auto result = RunKog({"discover", "--full", "--unregistered-depth", std::to_string(InDepth), "--format", "json", "--repo-root", InRoot.string(), "--no-cache"}, InRoot);
    RequireSuccess(result, "kog discover full json");
}

} // namespace

TEST_CASE("converge help distinguishes repos and branches taxonomy", "[tdd][functional][feature:converge][converge][help]") {
    const auto sandbox = CreateSandboxWorkspace("converge-help-taxonomy");

    const auto help = RunKog({"converge", "--help"}, sandbox.root);
    INFO(help.stdoutText);
    INFO(help.stderrText);
    REQUIRE(help.exitCode == 0);
    const auto helpText = help.stdoutText + "\n" + help.stderrText;
    RequireContains(helpText, "repos");
    RequireContains(helpText, "branches");
    RequireContains(helpText, "Converge repo state or branch state");

    const auto reposHelp = RunKog({"converge", "repos", "--help"}, sandbox.root);
    INFO(reposHelp.stdoutText);
    INFO(reposHelp.stderrText);
    REQUIRE(reposHelp.exitCode == 0);
    RequireContains(reposHelp.stdoutText + "\n" + reposHelp.stderrText, "existing repo-state converge workflow");

    const auto branchesHelp = RunKog({"converge", "branches", "plan", "--help"}, sandbox.root);
    INFO(branchesHelp.stdoutText);
    INFO(branchesHelp.stderrText);
    REQUIRE(branchesHelp.exitCode == 0);
    const auto branchesText = branchesHelp.stdoutText + "\n" + branchesHelp.stderrText;
    RequireContains(branchesText, "Read-only branch convergence planner");
    RequireContains(branchesText, "--strategy");
    RequireContains(branchesText, "cherry-pick");
    RequireNotContains(branchesText, "--status");
    RequireNotContains(branchesText, "--resume");
    RequireNotContains(branchesText, "--force-with-lease");

    const auto branchesApplyHelp = RunKog({"converge", "branches", "apply", "--help"}, sandbox.root);
    INFO(branchesApplyHelp.stdoutText);
    INFO(branchesApplyHelp.stderrText);
    REQUIRE(branchesApplyHelp.exitCode == 0);
    const auto branchesApplyText = branchesApplyHelp.stdoutText + "\n" + branchesApplyHelp.stderrText;
    RequireContains(branchesApplyText, "cherry-pick");
    RequireContains(branchesApplyText, "--branch");

    const auto branchesRootHelp = RunKog({"converge", "branches", "--help"}, sandbox.root);
    INFO(branchesRootHelp.stdoutText);
    INFO(branchesRootHelp.stderrText);
    REQUIRE(branchesRootHelp.exitCode == 0);
    const auto branchesRootText = branchesRootHelp.stdoutText + "\n" + branchesRootHelp.stderrText;
    RequireContains(branchesRootText, "inventory");
    RequireContains(branchesRootText, "status");
    RequireContains(branchesRootText, "apply");
    RequireContains(branchesRootText, "retire");

    const auto ignoredRepoStateFlag = RunKog({"converge", "branches", "plan", "--status", "--target", "main", "--json"}, sandbox.root);
    INFO(ignoredRepoStateFlag.stdoutText);
    INFO(ignoredRepoStateFlag.stderrText);
    REQUIRE(ignoredRepoStateFlag.exitCode != 0);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("converge repos alias matches legacy dry-run planner", "[tdd][functional][feature:converge-state][converge][planner][alias]") {
    const auto ctx = CreateRemoteWithClone("converge-repos-alias");
    const auto beforeStatus = GitStatusShort(ctx.cloneRepo);

    const auto legacy = RunConvergeDryRun(ctx.cloneRepo, {"--jobs", "1"});
    INFO(legacy.stdoutText);
    INFO(legacy.stderrText);
    REQUIRE(legacy.exitCode == 0);

    const auto alias = RunKog({"converge", "repos", "--dry-run", "--jobs", "1"}, ctx.cloneRepo);
    INFO(alias.stdoutText);
    INFO(alias.stderrText);
    REQUIRE(alias.exitCode == 0);

    REQUIRE(PlanPayload(alias.stdoutText) == PlanPayload(legacy.stdoutText));
    REQUIRE(GitStatusShort(ctx.cloneRepo) == beforeStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches planner emits stable default rebase JSON child first", "[tdd][functional][feature:converge][converge][branches][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-branches-plan-default");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for branch plan");

    const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesPlan\"");
    RequireContains(result.stdoutText, "\"schemaVersion\": 1");
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "\"targetBranch\": \"" + ctx.branch + "\"");
    RequireContains(result.stdoutText, "\"strategy\": \"rebase\"");
    RequireContains(result.stdoutText, "\"recursive\": true");
    RequireContains(result.stdoutText, "\"id\": \"" + ctx.submodulePath + "\"");
    RequireOrderedAfter(result.stdoutText, "\"traversalOrder\"", "\"" + ctx.submodulePath + "\"", "\".\"");
    RequireContains(result.stdoutText, "\"isTarget\": true");
    RequireContains(result.stdoutText, "\"name\": \"" + ctx.branch + "\"");
    RequireContains(result.stdoutText, "\"blockers\": []");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches inventory is read-only and reports blockers without blocked exit", "[tdd][functional][feature:converge][converge][branches][inventory]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-inventory");
    WriteTextFile(ctx.cloneRepo / "dirty.txt", "dirty\n");

    const auto result = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesInventory\"");
    RequireContains(result.stdoutText, "\"inventoryOnly\": true");
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "DIRTY_WORKTREE");
    REQUIRE(std::filesystem::exists(ctx.cloneRepo / "dirty.txt"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches inventory no-recursive avoids recursive status snapshot", "[functional][converge][branches][inventory][no-recursive][KG-BUG-0012]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-branches-inventory-no-recursive");
    const auto diagPath = (ctx.sandbox.root / "branch-inventory-no-recursive-process.log").lexically_normal();
    std::filesystem::remove(diagPath);

    const auto result = RunKogWithEnv(
        {"converge", "branches", "inventory", "--target", ctx.branch, "--json", "--jobs", "1", "--no-recursive"},
        ctx.cloneRootRepo,
        {
            {"KOG_PROCESS_DIAGNOSTICS_LOG", diagPath.string()},
            {"KOG_RECURSIVE_STATUS_DEADLINE_MS", "1"},
        });
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesInventory\"");
    RequireContains(result.stdoutText, "\"recursive\": false");
    RequireContains(result.stdoutText, "\"traversalOrder\"");
    RequireContains(result.stdoutText, "\"id\": \".\"");
    RequireNotContains(result.stdoutText, "\"" + ctx.submodulePath + "\"");
    RequireNotContains(result.stdoutText, "STATUS_SNAPSHOT_DEADLINE");

    REQUIRE(std::filesystem::exists(diagPath));
    const auto processDiag = ReadTextFile(diagPath);
    RequireNotContains(processDiag, "status --recursive");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches inventory skips gh-pages publish branch", "[functional][converge][branches][inventory][KG-BUG-0020]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-inventory-skips-gh-pages");

    RequireSuccess(RunGit({"checkout", "--orphan", "gh-pages"}, ctx.seedRepo), "checkout gh-pages publish branch");
    RequireSuccess(RunGit({"rm", "-rf", "."}, ctx.seedRepo), "clear publish branch tree");
    WriteTextFile(ctx.seedRepo / "index.html", "published docs\n");
    RequireSuccess(RunGit({"add", "index.html"}, ctx.seedRepo), "add publish branch content");
    RequireSuccess(RunGit({"commit", "-m", "publish docs"}, ctx.seedRepo), "commit publish branch");
    RequireSuccess(RunGit({"push", "origin", "gh-pages"}, ctx.seedRepo), "push publish branch");
    RequireSuccess(RunGit({"fetch", "origin", "refs/heads/gh-pages:refs/remotes/origin/gh-pages"}, ctx.cloneRepo), "fetch remote publish branch");

    const auto result = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--json", "--jobs", "1", "--no-recursive"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesInventory\"");
    RequireContains(result.stdoutText, "\"name\": \"origin/gh-pages\"");
    RequireContains(result.stdoutText, "\"nonConvergeTarget\": true");
    RequireContains(result.stdoutText, "\"skipReason\": \"PUBLISH_BRANCH\"");
    RequireContains(result.stdoutText, "skipped publish branch; not a coding convergence target");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches apply fast-forwards target and pushes", "[tdd][functional][feature:converge][converge][branches][apply]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-apply");
    const std::string featureBranch = "feature/apply-fast-forward";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout apply feature branch");
    WriteTextFile(ctx.cloneRepo / "feature.txt", "feature apply\n");
    RequireSuccess(RunGit({"add", "feature.txt"}, ctx.cloneRepo), "add apply feature file");
    RequireSuccess(RunGit({"commit", "-m", "apply feature"}, ctx.cloneRepo), "commit apply feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push apply feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before apply");

    const auto result = RunKog({"converge", "branches", "apply", "--target", ctx.branch, "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesApplyResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"fast-forward\"");
    REQUIRE(TrimCopy(RunGit({"rev-parse", ctx.branch}, ctx.cloneRepo).stdoutText) == featureHead);
    REQUIRE(TrimCopy(RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRepo).stdoutText) == featureHead);
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches apply cherry-picks selected non-ancestor branch", "[tdd][functional][feature:converge][converge][branches][apply][cherry-pick]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-apply-cherry-pick");
    const std::string featureBranch = "feature/apply-cherry-pick";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout cherry-pick feature branch");
    WriteTextFile(ctx.cloneRepo / "feature.txt", "feature cherry-pick\n");
    RequireSuccess(RunGit({"add", "feature.txt"}, ctx.cloneRepo), "add cherry-pick feature file");
    RequireSuccess(RunGit({"commit", "-m", "feature cherry-pick"}, ctx.cloneRepo), "commit cherry-pick feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push cherry-pick feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before cherry-pick apply");
    WriteTextFile(ctx.cloneRepo / "target.txt", "target branch advanced\n");
    RequireSuccess(RunGit({"add", "target.txt"}, ctx.cloneRepo), "add target branch file");
    RequireSuccess(RunGit({"commit", "-m", "advance target independently"}, ctx.cloneRepo), "commit target branch independently");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push independent target branch");

    const auto result = RunKog({"converge", "branches", "apply", "--target", ctx.branch, "--strategy", "cherry-pick", "--branch", featureBranch, "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesApplyResult\"");
    RequireContains(result.stdoutText, "\"strategy\": \"cherry-pick\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"cherry-pick\"");
    RequireContains(ReadTextFile(ctx.cloneRepo / "feature.txt"), "feature cherry-pick");
    REQUIRE(TrimCopy(RunGit({"rev-parse", ctx.branch}, ctx.cloneRepo).stdoutText) == TrimCopy(RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRepo).stdoutText));
    const auto cherry = RunGit({"cherry", ctx.branch, featureBranch}, ctx.cloneRepo);
    RequireSuccess(cherry, "git cherry after cherry-pick apply");
    RequireContains(cherry.stdoutText, "- " + featureHead);
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches apply cherry-picks past a clean source worktree", "[tdd][functional][feature:converge][converge][branches][apply][cherry-pick][worktree]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-apply-cherry-pick-source-worktree");
    const std::string featureBranch = "feature/apply-cherry-pick-source-worktree";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout source-worktree feature branch");
    WriteTextFile(ctx.cloneRepo / "source-worktree.txt", "source worktree cherry-pick\n");
    RequireSuccess(RunGit({"add", "source-worktree.txt"}, ctx.cloneRepo), "add source-worktree feature file");
    RequireSuccess(RunGit({"commit", "-m", "source worktree feature"}, ctx.cloneRepo), "commit source-worktree feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push source-worktree feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before adding source worktree");
    const auto worktreePath = (ctx.sandbox.root / "source-worktree-feature").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", worktreePath.string(), featureBranch}, ctx.cloneRepo), "add clean source branch worktree");

    WriteTextFile(ctx.cloneRepo / "target-source-worktree.txt", "target branch advanced\n");
    RequireSuccess(RunGit({"add", "target-source-worktree.txt"}, ctx.cloneRepo), "add target source-worktree file");
    RequireSuccess(RunGit({"commit", "-m", "advance target before source worktree apply"}, ctx.cloneRepo), "commit target before source-worktree apply");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target before source-worktree apply");

    const auto result = RunKog({"converge", "branches", "apply", "--target", ctx.branch, "--strategy", "cherry-pick", "--branch", featureBranch, "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesApplyResult\"");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"cherry-pick\"");
    RequireNotContains(result.stdoutText, "ACTIVE_WORKTREE_LEASE");
    RequireContains(ReadTextFile(ctx.cloneRepo / "source-worktree.txt"), "source worktree cherry-pick");
    REQUIRE(std::filesystem::exists(worktreePath));
    const auto cherry = RunGit({"cherry", ctx.branch, featureBranch}, ctx.cloneRepo);
    RequireSuccess(cherry, "git cherry after source-worktree cherry-pick apply");
    RequireContains(cherry.stdoutText, "- " + featureHead);
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches apply skips empty cherry-pick commits", "[tdd][functional][feature:converge][converge][branches][apply][cherry-pick]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-apply-cherry-pick-empty");
    const std::string featureBranch = "feature/apply-cherry-pick-empty";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout empty cherry-pick feature branch");
    RequireSuccess(RunGit({"commit", "--allow-empty", "-m", "empty cherry-pick feature"}, ctx.cloneRepo), "commit empty cherry-pick feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push empty cherry-pick feature");

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before empty cherry-pick apply");
    const auto targetHead = TrimCopy(RunGit({"rev-parse", ctx.branch}, ctx.cloneRepo).stdoutText);

    const auto result = RunKog({"converge", "branches", "apply", "--target", ctx.branch, "--strategy", "cherry-pick", "--branch", featureBranch, "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesApplyResult\"");
    RequireContains(result.stdoutText, "\"strategy\": \"cherry-pick\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"already-equivalent\"");
    REQUIRE(TrimCopy(RunGit({"rev-parse", ctx.branch}, ctx.cloneRepo).stdoutText) == targetHead);
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches apply reports cherry-pick conflicts without auto resolving", "[tdd][functional][feature:converge][converge][branches][apply][cherry-pick][conflict]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-apply-cherry-pick-conflict");
    WriteTextFile(ctx.cloneRepo / "conflict.txt", "base\n");
    RequireSuccess(RunGit({"add", "conflict.txt"}, ctx.cloneRepo), "add base conflict file");
    RequireSuccess(RunGit({"commit", "-m", "add conflict base"}, ctx.cloneRepo), "commit conflict base");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push conflict base");

    const std::string featureBranch = "feature/cherry-pick-conflict";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout conflict feature branch");
    WriteTextFile(ctx.cloneRepo / "conflict.txt", "feature\n");
    RequireSuccess(RunGit({"commit", "-am", "feature conflict change"}, ctx.cloneRepo), "commit feature conflict change");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push conflict feature branch");

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before conflict apply");
    WriteTextFile(ctx.cloneRepo / "conflict.txt", "target\n");
    RequireSuccess(RunGit({"commit", "-am", "target conflict change"}, ctx.cloneRepo), "commit target conflict change");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target conflict change");

    const auto result = RunKog({"converge", "branches", "apply", "--target", ctx.branch, "--strategy", "cherry-pick", "--branch", featureBranch, "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 1);
    RequireContains(result.stdoutText, "\"strategy\": \"cherry-pick\"");
    RequireContains(result.stdoutText, "CHERRY_PICK_CONFLICT");

    RequireSuccess(RunGit({"cherry-pick", "--abort"}, ctx.cloneRepo), "abort expected cherry-pick conflict");
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire removes merged branch and clean git worktree after confirmation", "[tdd][functional][feature:converge][converge][branches][retire]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire");
    const std::string featureBranch = "feature/retire-merged";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout retire feature branch");
    WriteTextFile(ctx.cloneRepo / "retire.txt", "retire me\n");
    RequireSuccess(RunGit({"add", "retire.txt"}, ctx.cloneRepo), "add retire feature file");
    RequireSuccess(RunGit({"commit", "-m", "retire feature"}, ctx.cloneRepo), "commit retire feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push retire feature");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before retire");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge retire feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push merged target");
    WriteTextFile(ctx.cloneRepo / "target-ahead.txt", "target ahead after merge\n");
    RequireSuccess(RunGit({"add", "target-ahead.txt"}, ctx.cloneRepo), "add target-ahead file");
    RequireSuccess(RunGit({"commit", "-m", "target ahead after retire merge"}, ctx.cloneRepo), "commit target ahead without push");
    const auto worktreePath = (ctx.sandbox.root / "retire-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", worktreePath.string(), featureBranch}, ctx.cloneRepo), "add clean feature worktree");

    const auto preview = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(preview.stdoutText);
    INFO(preview.stderrText);
    REQUIRE(preview.exitCode == 0);
    RequireContains(preview.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(preview.stdoutText, "\"mutationPerformed\": false");
    RequireContains(preview.stdoutText, "\"planned\"");
    RequireNotContains(preview.stdoutText, "DIRTY_WORKTREE:AHEAD_ONLY");
    REQUIRE(std::filesystem::exists(worktreePath));

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"delete-local\"");
    RequireNotContains(result.stdoutText, "DIRTY_WORKTREE:AHEAD_ONLY");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + featureBranch}, ctx.cloneRepo).exitCode != 0);
    REQUIRE(!std::filesystem::exists(worktreePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire allows merged branch with active clean worktree and branch-ahead upstream", "[tdd][functional][feature:converge][converge][branches][retire]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-merged-ahead");
    const std::string featureBranch = "feature/retire-merged-ahead";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout retire ahead feature branch");
    WriteTextFile(ctx.cloneRepo / "retire-ahead.txt", "first feature commit\n");
    RequireSuccess(RunGit({"add", "retire-ahead.txt"}, ctx.cloneRepo), "add first retire-ahead file");
    RequireSuccess(RunGit({"commit", "-m", "first retire ahead feature"}, ctx.cloneRepo), "commit first retire-ahead feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push first retire-ahead feature");
    WriteTextFile(ctx.cloneRepo / "retire-ahead.txt", "local feature commit already integrated\n");
    RequireSuccess(RunGit({"commit", "-am", "local retire ahead feature"}, ctx.cloneRepo), "commit local retire-ahead feature without pushing branch");

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before retire ahead");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge local-ahead feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target containing local-ahead feature");
    const auto worktreePath = (ctx.sandbox.root / "retire-ahead-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", worktreePath.string(), featureBranch}, ctx.cloneRepo), "add clean local-ahead feature worktree");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"delete-local\"");
    RequireNotContains(result.stdoutText, "ACTIVE_WORKTREE_LEASE");
    RequireNotContains(result.stdoutText, "UNPUSHED_COMMITS");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + featureBranch}, ctx.cloneRepo).exitCode != 0);
    REQUIRE(!std::filesystem::exists(worktreePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire removes patch-equivalent branch with clean active worktree", "[tdd][functional][feature:converge][converge][branches][retire][equivalent]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-patch-equivalent");
    const std::string featureBranch = "feature/retire-patch-equivalent";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout patch-equivalent feature branch");
    WriteTextFile(ctx.cloneRepo / "equivalent.txt", "patch equivalent branch\n");
    RequireSuccess(RunGit({"add", "equivalent.txt"}, ctx.cloneRepo), "add patch-equivalent file");
    RequireSuccess(RunGit({"commit", "-m", "patch equivalent feature"}, ctx.cloneRepo), "commit patch-equivalent feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push patch-equivalent feature");

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before patch-equivalent retire");
    WriteTextFile(ctx.cloneRepo / "target-independent.txt", "target independent commit\n");
    RequireSuccess(RunGit({"add", "target-independent.txt"}, ctx.cloneRepo), "add target independent file");
    RequireSuccess(RunGit({"commit", "-m", "target independent commit"}, ctx.cloneRepo), "commit target independently before patch-equivalent cherry-pick");
    RequireSuccess(RunGit({"cherry-pick", featureBranch}, ctx.cloneRepo), "cherry-pick feature onto target with different commit id");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target containing equivalent patch");
    const auto worktreePath = (ctx.sandbox.root / "retire-patch-equivalent-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", worktreePath.string(), featureBranch}, ctx.cloneRepo), "add clean patch-equivalent feature worktree");

    const auto inventory = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--strategy", "cherry-pick", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(inventory.stdoutText);
    INFO(inventory.stderrText);
    REQUIRE(inventory.exitCode == 0);
    RequireContains(inventory.stdoutText, "\"name\": \"" + featureBranch + "\"");
    RequireContains(inventory.stdoutText, "\"integrationProof\": \"patch-equivalent\"");
    RequireNotContains(inventory.stdoutText, "ACTIVE_WORKTREE_LEASE");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"integrationProof\": \"patch-equivalent\"");
    RequireContains(result.stdoutText, "\"action\": \"delete-local\"");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + featureBranch}, ctx.cloneRepo).exitCode != 0);
    REQUIRE(!std::filesystem::exists(worktreePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire removes merged detached worktree after confirmation", "[tdd][functional][feature:converge][converge][branches][retire][worktree]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-detached-worktree");
    const std::string featureBranch = "feature/retire-detached-worktree";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout detached-worktree feature branch");
    WriteTextFile(ctx.cloneRepo / "detached-worktree.txt", "detached worktree feature\n");
    RequireSuccess(RunGit({"add", "detached-worktree.txt"}, ctx.cloneRepo), "add detached-worktree feature file");
    RequireSuccess(RunGit({"commit", "-m", "detached worktree feature"}, ctx.cloneRepo), "commit detached-worktree feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push detached-worktree feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before detached-worktree retire");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge detached-worktree feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target containing detached-worktree feature");
    const auto worktreePath = (ctx.sandbox.root / "retire-detached-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), featureHead}, ctx.cloneRepo), "add clean detached worktree");

    const auto inventory = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--strategy", "cherry-pick", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(inventory.stdoutText);
    INFO(inventory.stderrText);
    REQUIRE(inventory.exitCode == 0);
    RequireContains(inventory.stdoutText, "\"detached\": true");
    RequireContains(inventory.stdoutText, "\"head\": \"" + featureHead + "\"");
    RequireContains(inventory.stdoutText, "\"integrationProof\": \"merged\"");
    RequireContains(inventory.stdoutText, "candidate for human-reviewed detached worktree retirement with --remove-worktrees");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"action\": \"remove-detached-worktree\"");
    RequireContains(result.stdoutText, "\"head\": \"" + featureHead + "\"");
    RequireContains(result.stdoutText, "\"integrationProof\": \"merged\"");
    REQUIRE(!std::filesystem::exists(worktreePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire blocks dirty detached worktree cleanup", "[tdd][functional][feature:converge][converge][branches][retire][worktree]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-dirty-detached-worktree");
    const std::string featureBranch = "feature/retire-dirty-detached-worktree";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout dirty detached-worktree feature branch");
    WriteTextFile(ctx.cloneRepo / "dirty-detached-worktree.txt", "clean feature content\n");
    RequireSuccess(RunGit({"add", "dirty-detached-worktree.txt"}, ctx.cloneRepo), "add dirty detached-worktree feature file");
    RequireSuccess(RunGit({"commit", "-m", "dirty detached worktree feature"}, ctx.cloneRepo), "commit dirty detached-worktree feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push dirty detached-worktree feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before dirty detached-worktree retire");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge dirty detached-worktree feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target containing dirty detached-worktree feature");
    const auto worktreePath = (ctx.sandbox.root / "retire-dirty-detached-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), featureHead}, ctx.cloneRepo), "add dirty detached worktree");
    WriteTextFile(worktreePath / "dirty-detached-worktree.txt", "dirty detached worktree content\n");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 1);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"action\": \"remove-detached-worktree\"");
    RequireContains(result.stdoutText, "\"head\": \"" + featureHead + "\"");
    RequireContains(result.stdoutText, "DIRTY_DETACHED_WORKTREE");
    REQUIRE(std::filesystem::exists(worktreePath));
    RequireContains(ReadTextFile(worktreePath / "dirty-detached-worktree.txt"), "dirty detached worktree content");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire harvests dirty detached ancestor worktree after confirmation", "[tdd][functional][feature:converge][converge][branches][retire][worktree][harvest]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-harvest-detached-worktree");
    const std::string featureBranch = "feature/harvest-detached-worktree";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout harvest detached-worktree feature branch");
    WriteTextFile(ctx.cloneRepo / "harvest-detached-worktree.txt", "clean feature content\n");
    RequireSuccess(RunGit({"add", "harvest-detached-worktree.txt"}, ctx.cloneRepo), "add harvest detached-worktree feature file");
    RequireSuccess(RunGit({"commit", "-m", "harvest detached worktree feature"}, ctx.cloneRepo), "commit harvest detached-worktree feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push harvest detached-worktree feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before harvest detached-worktree retire");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge harvest detached-worktree feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push target containing harvest detached-worktree feature");
    const auto worktreePath = (ctx.sandbox.root / "harvest-detached-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), featureHead}, ctx.cloneRepo), "add harvest dirty detached worktree");
    WriteTextFile(worktreePath / "harvest-detached-worktree.txt", "harvested detached worktree content\n");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--harvest-detached-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"action\": \"harvest-detached-worktree\"");
    RequireContains(result.stdoutText, "\"integrationProof\": \"merged\"");
    RequireContains(result.stdoutText, "\"harvested\"");
    REQUIRE(!std::filesystem::exists(worktreePath));
    RequireContains(ReadTextFile(ctx.cloneRepo / "harvest-detached-worktree.txt"), "harvested detached worktree content");
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto head = RunGit({"rev-parse", "HEAD"}, ctx.cloneRepo);
    const auto origin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRepo);
    RequireSuccess(head, "harvest rev-parse HEAD");
    RequireSuccess(origin, "harvest rev-parse origin");
    REQUIRE(head.stdoutText == origin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire blocks dirty detached non-ancestor harvest", "[tdd][functional][feature:converge][converge][branches][retire][worktree][harvest][blockers]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-harvest-non-ancestor");
    const std::string featureBranch = "feature/harvest-non-ancestor";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout non-ancestor detached feature branch");
    WriteTextFile(ctx.cloneRepo / "non-ancestor-detached-worktree.txt", "feature content\n");
    RequireSuccess(RunGit({"add", "non-ancestor-detached-worktree.txt"}, ctx.cloneRepo), "add non-ancestor detached feature file");
    RequireSuccess(RunGit({"commit", "-m", "non ancestor detached feature"}, ctx.cloneRepo), "commit non-ancestor detached feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push non-ancestor detached feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before non-ancestor detached worktree");
    const auto worktreePath = (ctx.sandbox.root / "harvest-non-ancestor-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), featureHead}, ctx.cloneRepo), "add non-ancestor dirty detached worktree");
    WriteTextFile(worktreePath / "non-ancestor-detached-worktree.txt", "dirty non-ancestor detached content\n");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--harvest-detached-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 1);

    RequireContains(result.stdoutText, "\"action\": \"harvest-detached-worktree\"");
    RequireContains(result.stdoutText, "DIRTY_DETACHED_WORKTREE_NON_ANCESTOR");
    REQUIRE(std::filesystem::exists(worktreePath));
    RequireContains(ReadTextFile(worktreePath / "non-ancestor-detached-worktree.txt"), "dirty non-ancestor detached content");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire prunes stale worktree metadata after confirmation", "[tdd][functional][feature:converge][converge][branches][retire][worktree][prune]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-prune-stale-worktree");
    const auto worktreePath = (ctx.sandbox.root / "stale-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), "HEAD"}, ctx.cloneRepo), "add stale worktree fixture");
    REQUIRE(std::filesystem::exists(worktreePath));
    std::filesystem::remove_all(worktreePath);

    const auto before = RunGit({"worktree", "list", "--porcelain"}, ctx.cloneRepo);
    RequireSuccess(before, "worktree list before prune");
    RequireContains(before.stdoutText, worktreePath.generic_string());

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--prune-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "\"action\": \"worktree-prune\"");

    const auto after = RunGit({"worktree", "list", "--porcelain"}, ctx.cloneRepo);
    RequireSuccess(after, "worktree list after prune");
    RequireNotContains(after.stdoutText, worktreePath.generic_string());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime settle removes clean detached worktree", "[tdd][functional][feature:converge][converge][branches][retire][worktree][settle]") {
    const auto ctx = CreateRemoteWithClone("converge-runtime-settle-clean-detached-worktree");
    const std::string featureBranch = "feature/runtime-settle-detached-worktree";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout runtime settle feature branch");
    WriteTextFile(ctx.cloneRepo / "runtime-settle-detached-worktree.txt", "runtime settle feature\n");
    RequireSuccess(RunGit({"add", "runtime-settle-detached-worktree.txt"}, ctx.cloneRepo), "add runtime settle feature file");
    RequireSuccess(RunGit({"commit", "-m", "runtime settle detached feature"}, ctx.cloneRepo), "commit runtime settle feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push runtime settle feature");
    const auto featureHead = TrimCopy(RunGit({"rev-parse", featureBranch}, ctx.cloneRepo).stdoutText);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before runtime settle");
    RequireSuccess(RunGit({"merge", "--ff-only", featureBranch}, ctx.cloneRepo), "merge runtime settle feature into target");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push runtime settle target");
    const auto worktreePath = (ctx.sandbox.root / "runtime-settle-clean-detached-worktree").lexically_normal();
    RequireSuccess(RunGit({"worktree", "add", "--detach", worktreePath.string(), featureHead}, ctx.cloneRepo), "add runtime settle clean detached worktree");

    const auto result = RunKog({"converge", "--settle-worktrees", "--remove-worktrees", "--target", ctx.branch, "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "[converge] phase=settle-worktrees");
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(!std::filesystem::exists(worktreePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire removes empty cherry-pick no-op branch", "[tdd][functional][feature:converge][converge][branches][retire][equivalent]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-empty-noop");
    const std::string featureBranch = "feature/retire-empty-noop";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout empty-noop feature branch");
    RequireSuccess(RunGit({"commit", "--allow-empty", "-m", "empty no-op feature"}, ctx.cloneRepo), "commit empty-noop feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push empty-noop feature");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before empty-noop retire");

    const auto inventory = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--strategy", "cherry-pick", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(inventory.stdoutText);
    INFO(inventory.stderrText);
    REQUIRE(inventory.exitCode == 0);
    RequireContains(inventory.stdoutText, "\"name\": \"" + featureBranch + "\"");
    RequireContains(inventory.stdoutText, "\"integrationProof\": \"cherry-pick-noop\"");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"integrationProof\": \"cherry-pick-noop\"");
    RequireContains(result.stdoutText, "\"action\": \"delete-local\"");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + featureBranch}, ctx.cloneRepo).exitCode != 0);
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire deletes remote-only no-op branches with explicit remote confirmation", "[tdd][functional][feature:converge][converge][branches][retire][remote-only]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-remote-only-noop");
    const std::string featureBranch = "feature/retire-remote-only-noop";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout remote-only noop feature branch");
    RequireSuccess(RunGit({"commit", "--allow-empty", "-m", "remote-only empty no-op feature"}, ctx.cloneRepo), "commit remote-only noop feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push remote-only noop feature");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before remote-only retire");
    RequireSuccess(RunGit({"branch", "-D", featureBranch}, ctx.cloneRepo), "delete local remote-only noop branch");

    const auto inventory = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--strategy", "cherry-pick", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(inventory.stdoutText);
    INFO(inventory.stderrText);
    REQUIRE(inventory.exitCode == 0);
    RequireContains(inventory.stdoutText, "\"name\": \"origin/" + featureBranch + "\"");
    RequireContains(inventory.stdoutText, "\"remoteOnly\": true");
    RequireContains(inventory.stdoutText, "\"remoteBranch\": \"" + featureBranch + "\"");
    RequireContains(inventory.stdoutText, "\"integrationProof\": \"cherry-pick-noop\"");

    const auto blocked = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(blocked.stdoutText);
    INFO(blocked.stderrText);
    REQUIRE(blocked.exitCode == 1);
    RequireContains(blocked.stdoutText, "REMOTE_DELETE_REQUIRED");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--delete-remote", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"origin/" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"action\": \"delete-remote\"");
    RequireContains(result.stdoutText, "\"remoteOnly\": true");
    const auto lsRemote = RunGit({"ls-remote", "--heads", "origin", featureBranch}, ctx.cloneRepo);
    RequireSuccess(lsRemote, "ls-remote after remote-only retire");
    REQUIRE(TrimCopy(lsRemote.stdoutText).empty());
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches retire proves empty no-op branch with untracked target dirt", "[tdd][functional][feature:converge][converge][branches][retire][equivalent][dirty]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-retire-empty-noop-untracked");
    const std::string featureBranch = "feature/retire-empty-noop-untracked";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout untracked-noop feature branch");
    RequireSuccess(RunGit({"commit", "--allow-empty", "-m", "empty no-op feature with untracked target"}, ctx.cloneRepo), "commit untracked-noop feature");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push untracked-noop feature");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target before untracked-noop retire");
    WriteTextFile(ctx.cloneRepo / "local-scratch.tmp", "local scratch stays untracked\n");

    const auto inventory = RunKog({"converge", "branches", "inventory", "--target", ctx.branch, "--strategy", "cherry-pick", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(inventory.stdoutText);
    INFO(inventory.stderrText);
    REQUIRE(inventory.exitCode == 0);
    RequireContains(inventory.stdoutText, "\"name\": \"" + featureBranch + "\"");
    RequireContains(inventory.stdoutText, "\"integrationProof\": \"cherry-pick-noop\"");

    const auto result = RunKog({"converge", "branches", "retire", "--target", ctx.branch, "--remove-worktrees", "--confirm", "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesRetireResult\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": true");
    RequireContains(result.stdoutText, "\"branch\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"integrationProof\": \"cherry-pick-noop\"");
    REQUIRE(RunGit({"show-ref", "--verify", "--quiet", "refs/heads/" + featureBranch}, ctx.cloneRepo).exitCode != 0);
    RequireContains(GitStatusShort(ctx.cloneRepo), "?? local-scratch.tmp");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches planner records explicit merge override in agent JSON", "[tdd][functional][feature:converge][converge][branches][planner][agent-mode]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-plan-merge");
    RequireSuccess(RunGit({"checkout", "-b", "feature/merge-plan"}, ctx.cloneRepo), "checkout feature branch");
    WriteTextFile(ctx.cloneRepo / "feature.txt", "feature work\n");
    RequireSuccess(RunGit({"add", "feature.txt"}, ctx.cloneRepo), "add feature file");
    RequireSuccess(RunGit({"commit", "-m", "feature branch work"}, ctx.cloneRepo), "commit feature branch");
    RequireSuccess(RunGit({"push", "-u", "origin", "feature/merge-plan"}, ctx.cloneRepo), "push feature branch for clean merge plan");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target branch");

    const auto result = RunKogWithEnv(
        {"converge", "branches", "plan", "--target", ctx.branch, "--strategy", "merge", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);

    RequireContains(result.stdoutText, "\"schemaName\": \"kog.convergeBranchesPlan\"");
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "\"strategy\": \"merge\"");
    RequireContains(result.stdoutText, "\"mergedIntoTarget\": false");
    RequireContains(result.stdoutText, "\"name\": \"feature/merge-plan\"");
    RequireContains(result.stdoutText, "\"blockers\": []");
    RequireContains(result.stdoutText, "would plan merge integration into " + ctx.branch);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches planner blocks stale target branch behind upstream", "[tdd][functional][feature:converge][converge][branches][planner][blockers]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-plan-stale-target");
    CommitAndPushFile(ctx.seedRepo, ctx.branch, "remote-main.txt", "remote main\n", "advance remote main");
    RequireSuccess(RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo), "fetch advanced target remote");

    const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 1);
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "\"name\": \"" + ctx.branch + "\"");
    RequireContains(result.stdoutText, "\"behind\": 1");
    RequireContains(result.stdoutText, "STALE_TARGET_BRANCH");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches planner blocks stale non-target branch behind upstream", "[tdd][functional][feature:converge][converge][branches][planner][blockers]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-plan-stale-branch");
    const std::string featureBranch = "feature/stale-local";
    RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.seedRepo), "create seed feature branch");
    WriteTextFile(ctx.seedRepo / "feature.txt", "feature seed\n");
    RequireSuccess(RunGit({"add", "feature.txt"}, ctx.seedRepo), "add seed feature file");
    RequireSuccess(RunGit({"commit", "-m", "seed feature branch"}, ctx.seedRepo), "commit seed feature branch");
    RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.seedRepo), "push seed feature branch");
    RequireSuccess(RunGit({"fetch", "origin", featureBranch}, ctx.cloneRepo), "fetch seed feature branch");
    RequireSuccess(RunGit({"checkout", "-b", featureBranch, "origin/" + featureBranch}, ctx.cloneRepo), "checkout local feature branch");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target branch");

    CommitAndPushFile(ctx.seedRepo, featureBranch, "feature-remote.txt", "feature remote\n", "advance remote feature");
    RequireSuccess(RunGit({"fetch", "origin", featureBranch}, ctx.cloneRepo), "fetch advanced feature remote");

    const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 1);
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");
    RequireContains(result.stdoutText, "\"name\": \"" + featureBranch + "\"");
    RequireContains(result.stdoutText, "\"behind\": 1");
    RequireContains(result.stdoutText, "STALE_LOCAL_BRANCH");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge branches planner blocks dirty unpushed and missing target cases", "[tdd][functional][feature:converge][converge][branches][planner][blockers]") {
    {
        const auto ctx = CreateRemoteWithClone("converge-branches-plan-dirty-blocker");
        WriteTextFile(ctx.cloneRepo / "dirty.txt", "dirty\n");

        const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 1);
        RequireContains(result.stdoutText, "\"mutationPerformed\": false");
        RequireContains(result.stdoutText, "DIRTY_WORKTREE:");
        RemoveSandboxWorkspace(ctx.sandbox);
    }
    {
        const auto ctx = CreateRemoteWithClone("converge-branches-plan-unpushed-blocker");
        const std::string featureBranch = "feature/ahead-local";
        RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout local ahead feature branch");
        WriteTextFile(ctx.cloneRepo / "ahead.txt", "ahead seed\n");
        RequireSuccess(RunGit({"add", "ahead.txt"}, ctx.cloneRepo), "add ahead file");
        RequireSuccess(RunGit({"commit", "-m", "seed ahead feature"}, ctx.cloneRepo), "commit ahead feature");
        RequireSuccess(RunGit({"push", "-u", "origin", featureBranch}, ctx.cloneRepo), "push initial ahead feature");
        WriteTextFile(ctx.cloneRepo / "ahead.txt", "ahead local\n");
        RequireSuccess(RunGit({"commit", "-am", "local ahead feature"}, ctx.cloneRepo), "commit local ahead feature");
        RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target branch");

        const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 1);
        RequireContains(result.stdoutText, "\"name\": \"" + featureBranch + "\"");
        RequireContains(result.stdoutText, "UNPUSHED_COMMITS");
        RemoveSandboxWorkspace(ctx.sandbox);
    }
    {
        const auto ctx = CreateRemoteWithClone("converge-branches-plan-local-only-blocker");
        const std::string featureBranch = "feature/local-only";
        RequireSuccess(RunGit({"checkout", "-b", featureBranch}, ctx.cloneRepo), "checkout local-only feature branch");
        WriteTextFile(ctx.cloneRepo / "local-only.txt", "local only\n");
        RequireSuccess(RunGit({"add", "local-only.txt"}, ctx.cloneRepo), "add local-only file");
        RequireSuccess(RunGit({"commit", "-m", "local-only feature"}, ctx.cloneRepo), "commit local-only feature");
        RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "return to target branch");

        const auto result = RunKog({"converge", "branches", "plan", "--target", ctx.branch, "--json", "--jobs", "1"}, ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 1);
        RequireContains(result.stdoutText, "\"name\": \"" + featureBranch + "\"");
        RequireContains(result.stdoutText, "\"hasUpstream\": false");
        RequireContains(result.stdoutText, "UNPUSHED_COMMITS");
        RemoveSandboxWorkspace(ctx.sandbox);
    }
    {
        const auto ctx = CreateRemoteWithClone("converge-branches-plan-missing-target-blocker");
        const auto result = RunKog({"converge", "branches", "plan", "--target", "missing-target", "--json", "--jobs", "1"}, ctx.cloneRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode == 1);
        RequireContains(result.stdoutText, "TARGET_REF_MISSING");
        RemoveSandboxWorkspace(ctx.sandbox);
    }
}

TEST_CASE("converge branches planner emits clean agent JSON without command log preamble", "[tdd][functional][feature:converge][converge][branches][planner][agent-mode]") {
    const auto ctx = CreateRemoteWithClone("converge-branches-plan-agent-json-clean");

    const auto result = RunKogWithEnv(
        {"converge", "branches", "plan", "--target", ctx.branch, "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireNotContains(result.stdoutText, "[run]");
    RequireNotContains(result.stdoutText, "[TIMING]");
    RequireContains(result.stdoutText, "\"mutationPerformed\": false");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner dry-run prints deterministic executable plan", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-plan");
    const auto beforeStatus = GitStatusShort(ctx.cloneRepo);

    const auto jobs1 = RunConvergeDryRun(ctx.cloneRepo, {"--jobs", "1"});
    INFO(jobs1.stdoutText);
    INFO(jobs1.stderrText);
    REQUIRE(jobs1.exitCode == 0);
    RequireContains(jobs1.stdoutText, "Converge Plan");
    RequireContains(jobs1.stdoutText, "Status preflight counts");
    RequireContains(jobs1.stdoutText, "Dependency waves");
    RequireContains(jobs1.stdoutText, "Phase sync actions");
    RequireContains(jobs1.stdoutText, "Phase commit actions");
    RequireContains(jobs1.stdoutText, "Phase push actions");
    RequireContains(jobs1.stdoutText, "Skipped repos");
    RequireContains(jobs1.stdoutText, "commit skipped: CLEAN");
    RequireContains(jobs1.stdoutText, "unregisteredScan=disabled");

    const auto jobs4 = RunConvergeDryRun(ctx.cloneRepo, {"--jobs", "4"});
    INFO(jobs4.stdoutText);
    INFO(jobs4.stderrText);
    REQUIRE(jobs4.exitCode == 0);
    REQUIRE(PlanPayload(jobs1.stdoutText) == PlanPayload(jobs4.stdoutText));
    REQUIRE(GitStatusShort(ctx.cloneRepo) == beforeStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner gitlink only uses deterministic pointer commit", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-gitlink");
    const auto beforeRootStatus = GitStatusShort(ctx.cloneRootRepo);
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for pointer commit test");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child moved\n");
    RequireSuccess(RunGit({"commit", "-am", "move child"}, ctx.cloneChildRepo), "commit child move");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneChildRepo), "push child move");
    const auto dirtyRootStatus = GitStatusShort(ctx.cloneRootRepo);
    REQUIRE(dirtyRootStatus != beforeRootStatus);

    const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "GITLINK_DIRTY_ONLY");
    RequireContains(result.stdoutText, "kog commit -ai skipped: GITLINK_DIRTY_ONLY");
    RequireContains(result.stdoutText, "deterministic pointer commit: [Submodule][Chore] Update Build/Base pointer (NO-TICKET)");
    REQUIRE(GitStatusShort(ctx.cloneRootRepo) == dirtyRootStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner defers unsafe parent pointer while child can converge", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-defer-parent-unsafe");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for deferred parent test");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child seed\nlocal child change\n");
    const auto dirtyRootStatus = GitStatusShort(ctx.cloneRootRepo);

    const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "GITLINK_DIRTY_UNSAFE");
    RequireContains(result.stdoutText, ctx.submodulePath + ": kog commit -ai --repos " + ctx.submodulePath);
    RequireContains(result.stdoutText, ".: parent pointer commit waits for child worktree converge");
    RequireNotContains(result.stdoutText, "PARENT_POINTER_UNSAFE");
    REQUIRE(GitStatusShort(ctx.cloneRootRepo) == dirtyRootStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime commits dirty child before parent pointer", "[tdd][functional][feature:converge-state][feature:dirty-kind][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-runtime-child-before-parent");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for runtime child converge");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child seed\nruntime child change\n");

    const auto result = RunKog({"converge", "--jobs", "1"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "[converge] phase=commit-local-changes-if-needed");
    RequireContains(result.stdoutText, "[converge] phase=commit-pointer-updates-if-needed");
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(ctx.cloneChildRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneRootRepo).empty());

    const auto childHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneChildRepo);
    const auto childOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneChildRepo);
    const auto rootHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneRootRepo);
    const auto rootOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRootRepo);
    RequireSuccess(childHead, "child rev-parse HEAD");
    RequireSuccess(childOrigin, "child rev-parse origin");
    RequireSuccess(rootHead, "root rev-parse HEAD");
    RequireSuccess(rootOrigin, "root rev-parse origin");
    REQUIRE(childHead.stdoutText == childOrigin.stdoutText);
    REQUIRE(rootHead.stdoutText == rootOrigin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge commits deferred parent content before refreshed sync and push", "[tdd][functional][feature:converge-state][feature:dirty-kind][converge][planner][agent-mode][KG-BUG-0032]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-deferred-parent-content-diverged");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for deferred parent content test");

    WriteTextFile(ctx.rootSeedRepo / "remote-root.txt", "remote root change\n");
    RequireSuccess(RunGit({"add", "remote-root.txt"}, ctx.rootSeedRepo), "stage remote root change");
    RequireSuccess(RunGit({"commit", "-m", "remote root change"}, ctx.rootSeedRepo), "commit remote root change");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.rootSeedRepo), "push remote root change");

    WriteTextFile(ctx.cloneRootRepo / "local-root.txt", "local root commit\n");
    RequireSuccess(RunGit({"add", "local-root.txt"}, ctx.cloneRootRepo), "stage local root commit");
    RequireSuccess(RunGit({"commit", "-m", "local root commit"}, ctx.cloneRootRepo), "commit local root commit");
    RequireSuccess(RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRootRepo), "fetch remote root change");

    const auto deferredRootContent = ctx.cloneRootRepo / "docs" / "deferred-after-child.md";
    std::filesystem::create_directories(deferredRootContent.parent_path());
    const auto childGitDirResult = RunGit({"rev-parse", "--absolute-git-dir"}, ctx.cloneChildRepo);
    RequireSuccess(childGitDirResult, "resolve child git directory for post-commit fixture");
    WriteTextFile(
        std::filesystem::path(TrimCopy(childGitDirResult.stdoutText)) / "hooks" / "post-commit",
        "#!/bin/sh\nprintf 'deferred root content\\n' > '" + deferredRootContent.generic_string() + "'\n");
    WriteTextFile(ctx.cloneChildRepo / "docs" / "child.md", "deferred child content\n");

    const auto result = RunKogWithEnv(
        {"converge", "--jobs", "1"},
        ctx.cloneRootRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "include docs/deferred-after-child.md");
    RequireContains(result.stdoutText, "[converge] sync_repo=.");

    const auto deltaPhase = result.stdoutText.find("[converge] phase=status-delta-after-sync");
    const auto deferredCommitPhase = result.stdoutText.find("[converge] phase=commit-pointer-updates-if-needed");
    const auto refreshedSyncPhase = result.stdoutText.find("[converge] phase=sync-converge-dependent-repos");
    const auto parentPushPhase = result.stdoutText.find("[converge] phase=push-parents-bottom-up");
    REQUIRE(deltaPhase < deferredCommitPhase);
    REQUIRE(deferredCommitPhase < refreshedSyncPhase);
    REQUIRE(refreshedSyncPhase < parentPushPhase);

    REQUIRE(GitStatusShort(ctx.cloneChildRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneRootRepo).empty());
    const auto childHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneChildRepo);
    const auto childOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneChildRepo);
    const auto rootHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneRootRepo);
    const auto rootOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRootRepo);
    RequireSuccess(childHead, "child rev-parse HEAD after deferred parent converge");
    RequireSuccess(childOrigin, "child rev-parse origin after deferred parent converge");
    RequireSuccess(rootHead, "root rev-parse HEAD after deferred parent converge");
    RequireSuccess(rootOrigin, "root rev-parse origin after deferred parent converge");
    REQUIRE(childHead.stdoutText == childOrigin.stdoutText);
    REQUIRE(rootHead.stdoutText == rootOrigin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge no-recursive scopes planner and runtime to current repo", "[tdd][functional][feature:converge-state][converge][planner][no-recursive]") {
    const auto plannerCtx = CreateRemoteWithSubmoduleClone("converge-no-recursive-plan");
    WriteTextFile(plannerCtx.cloneRootRepo / "root-only.txt", "current repo change\n");

    const auto planResult = RunConvergeDryRun(plannerCtx.cloneRootRepo, {"--no-recursive", "--jobs", "1"});
    INFO(planResult.stdoutText);
    INFO(planResult.stderrText);
    REQUIRE(planResult.exitCode == 0);
    const auto plan = PlanPayload(planResult.stdoutText);
    RequireContains(plan, "repos=1 dirty=1");
    RequireContains(plan, ".: kog commit -ai --repos .");
    RequireNotContains(plan, plannerCtx.submodulePath);

    RemoveSandboxWorkspace(plannerCtx.sandbox);

    const auto runtimeCtx = CreateRemoteWithClone("converge-no-recursive-runtime");
    WriteTextFile(runtimeCtx.cloneRepo / "runtime.txt", "runtime current repo change\n");

    const auto result = RunKog({"converge", "--no-recursive", "--jobs", "1"}, runtimeCtx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(runtimeCtx.cloneRepo).empty());

    const auto head = RunGit({"rev-parse", "HEAD"}, runtimeCtx.cloneRepo);
    const auto origin = RunGit({"rev-parse", "origin/" + runtimeCtx.branch}, runtimeCtx.cloneRepo);
    RequireSuccess(head, "runtime no-recursive rev-parse HEAD");
    RequireSuccess(origin, "runtime no-recursive rev-parse origin");
    REQUIRE(head.stdoutText == origin.stdoutText);

    RemoveSandboxWorkspace(runtimeCtx.sandbox);
}

TEST_CASE("converge agent mode commits backlog changes by inferred intent", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-intent-commits");

    const auto kgItem = std::filesystem::path("products/kano-git-master-skill/items/bug/0000/KG-BUG-0001_make-kog-converge-agent-mode-create-intent-scoped.md");
    const auto kgEvidence = std::filesystem::path("products/kano-git-master-skill/artifacts/KG-BUG-0001/verification/evidence.txt");
    const auto kgView = std::filesystem::path("products/kano-git-master-skill/views/Dashboard_PlainMarkdown_Active.md");
    const auto koaItem = std::filesystem::path("products/kano-agent-ark-skill/items/task/0100/KOA-TSK-0130_update-runtime.md");
    const auto koaReceipt = std::filesystem::path("products/kano-agent-ark-skill/artifacts/_receipts/backlog.item_create/implicit-demo.json");
    const auto koaTopic = std::filesystem::path("products/kano-agent-ark-skill/topics/topic-runtime/topic.json");
    const auto koaTopicNode = std::filesystem::path("products/kano-agent-ark-skill/topics/topic-runtime/graph/nodes/WO-KOA-TSK-0130-demo.json");
    const auto koaRunnerWake = std::filesystem::path("products/kano-agent-ark-skill/artifacts/_runner-wake/codex.jsonl");

    WriteTextFile(ctx.seedRepo / kgItem, "id: KG-BUG-0001\nstate: Proposed\n");
    WriteTextFile(ctx.seedRepo / kgEvidence, "initial evidence\n");
    WriteTextFile(ctx.seedRepo / kgView, "# Active\n");
    WriteTextFile(ctx.seedRepo / koaItem, "id: KOA-TSK-0130\nstate: Proposed\n");
    RequireSuccess(RunGit({"add", "products"}, ctx.seedRepo), "seed tracked backlog files");
    RequireSuccess(RunGit({"commit", "-m", "seed backlog files"}, ctx.seedRepo), "commit tracked backlog files");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push tracked backlog files");
    RequireSuccess(RunGit({"pull", "--rebase", "origin", ctx.branch}, ctx.cloneRepo), "pull tracked backlog files");

    WriteTextFile(ctx.cloneRepo / kgItem, "id: KG-BUG-0001\nstate: InProgress\nnotes: converge should split this intent\n");
    WriteTextFile(ctx.cloneRepo / kgEvidence, "initial evidence\nverified agent intent plan\n");
    WriteTextFile(ctx.cloneRepo / kgView, "# Active\n\n- KG-BUG-0001\n");
    WriteTextFile(ctx.cloneRepo / koaItem, "id: KOA-TSK-0130\nstate: Ready\n");
    WriteTextFile(ctx.cloneRepo / koaReceipt, "{\"ok\":true}\n");
    WriteTextFile(ctx.cloneRepo / koaTopic, "{\"id\":\"topic-runtime\"}\n");
    WriteTextFile(ctx.cloneRepo / koaTopicNode, "{\"id\":\"WO-KOA-TSK-0130-demo\"}\n");
    WriteTextFile(ctx.cloneRepo / koaRunnerWake, "{\"event\":\"wake\"}\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[Backlog][Docs] Update KG-BUG-0001 bug item (KG-BUG-0001)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Add KG-BUG-0001 evidence (KG-BUG-0001)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Refresh kano-git-master-skill backlog views (NO-TICKET)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Update KOA-TSK-0130 task item (KOA-TSK-0130)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Add kano-agent-ark-skill mutation receipts (NO-TICKET)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Update kano-agent-ark-skill backlog topics (NO-TICKET)");
    RequireContains(result.stdoutText, "[Backlog][Docs] Update kano-agent-ark-skill backlog artifacts (NO-TICKET)");
    RequireNotContains(result.stdoutText, "docs(backlog-");
    RequireNotContains(result.stdoutText, "ambiguous " + koaTopic.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + koaRunnerWake.generic_string());
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto log = RunGit({"log", "--format=%s", "-n", "8"}, ctx.cloneRepo);
    RequireSuccess(log, "read converge commit log");
    RequireContains(log.stdoutText, "[Backlog][Docs] Update KG-BUG-0001 bug item (KG-BUG-0001)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Add KG-BUG-0001 evidence (KG-BUG-0001)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Refresh kano-git-master-skill backlog views (NO-TICKET)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Update KOA-TSK-0130 task item (KOA-TSK-0130)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Add kano-agent-ark-skill mutation receipts (NO-TICKET)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Update kano-agent-ark-skill backlog topics (NO-TICKET)");
    RequireContains(log.stdoutText, "[Backlog][Docs] Update kano-agent-ark-skill backlog artifacts (NO-TICKET)");
    RequireNotContains(log.stdoutText, "docs(backlog-");
    RequireNotContains(log.stdoutText, "update 4 files");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode coalesces classified pre-staged paths before remaining intent groups", "[functional][converge][agent-mode][intent-commits][index][KG-BUG-0024]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-pre-staged-intents");
    const auto documentation = std::filesystem::path("docs/operator-staged.md");
    const auto configuration = std::filesystem::path("config/operator-staged.toml");
    const auto workflowTemplate = std::filesystem::path("templates/feature/remaining.md.template");

    WriteTextFile(ctx.cloneRepo / documentation, "# staged documentation\n");
    WriteTextFile(ctx.cloneRepo / configuration, "enabled = true\n");
    RequireSuccess(
        RunGit({"add", documentation.generic_string(), configuration.generic_string()}, ctx.cloneRepo),
        "stage paths from separate intent groups");
    WriteTextFile(ctx.cloneRepo / workflowTemplate, "# remaining unstaged intent\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "[Converge][Chore] Commit pre-staged intent changes (NO-TICKET)");
    RequireContains(result.stdoutText, "include " + documentation.generic_string());
    RequireContains(result.stdoutText, "include " + configuration.generic_string());
    RequireContains(result.stdoutText, "[KOG][Docs] Update workflow templates (NO-TICKET)");
    RequireContains(result.stdoutText, "include " + workflowTemplate.generic_string());
    RequireNotContains(result.stdoutText, "pre-existing staged path outside plan include/exclude scope");
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto log = RunGit({"log", "--format=%s", "-n", "3"}, ctx.cloneRepo);
    RequireSuccess(log, "read pre-staged converge commit log");
    RequireContains(log.stdoutText, "[Converge][Chore] Commit pre-staged intent changes (NO-TICKET)");
    RequireContains(log.stdoutText, "[KOG][Docs] Update workflow templates (NO-TICKET)");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode defers registered child paths from root intent plan", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits][gitlink]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-agent-root-content-and-child");
    const auto childDoc = std::filesystem::path("docs/child.md");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for agent-mode root intent test");

    WriteTextFile(ctx.cloneRootRepo / ".gitignore", ".kano/\nlocal-cache/\n");
    WriteTextFile(ctx.cloneChildRepo / childDoc, "agent mode child change\n");

    const auto result = RunKogWithEnv(
        {"converge", "--jobs", "1"},
        ctx.cloneRootRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[KOG][Chore] Update repository policy (NO-TICKET)");
    RequireNotContains(result.stdoutText, "[plan][fingerprint] start");
    RequireNotContains(result.stdoutText, "ambiguous " + ctx.submodulePath);
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(ctx.cloneChildRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneRootRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode classifies staged gitmodules as repository policy", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits][KG-BUG-0033]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-gitmodules-policy");
    WriteTextFile(ctx.cloneRepo / ".gitmodules", "[submodule \"deps/example\"]\n\tpath = deps/example\n\turl = https://example.invalid/example.git\n");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.cloneRepo), "stage gitmodules policy");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[Converge][Chore] Commit pre-staged intent changes (NO-TICKET)");
    RequireContains(result.stdoutText, "include .gitmodules");
    RequireNotContains(result.stdoutText, "ambiguous .gitmodules");
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode keeps implementation and test paths in one source intent", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-source-intent");

    const auto implementation = std::filesystem::path("src/cpp/code/systems/kano_git_command/repo_sync/private/converge_cmd.cpp");
    const auto regressionTest = std::filesystem::path("src/cpp/code/tests/kano_git_cli_tests/functional/converge_planner_test.cpp");
    const auto repoConfig = std::filesystem::path("config/repo-catalog.toml");
    const auto composeConfig = std::filesystem::path("docker-compose.webview.yml");
    const auto workflowTemplate = std::filesystem::path("templates/feature/notes.md.template");
    const auto powerShellCache = std::filesystem::path("Microsoft/Windows/PowerShell/ModuleAnalysisCache");
    const auto shellLog = std::filesystem::path("$log");
    const auto nulDiff = std::filesystem::path("NUL_diff.txt");
    const auto topicActive = std::filesystem::path("topics/2026-07-10-platform-budget-wave/.active");

    WriteTextFile(ctx.seedRepo / implementation, "// converge implementation\n");
    WriteTextFile(ctx.seedRepo / regressionTest, "// converge tests\n");
    RequireSuccess(RunGit({"add", "src"}, ctx.seedRepo), "seed tracked source files");
    RequireSuccess(RunGit({"commit", "-m", "seed converge source files"}, ctx.seedRepo), "commit tracked source files");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push tracked source files");
    RequireSuccess(RunGit({"pull", "--rebase", "origin", ctx.branch}, ctx.cloneRepo), "pull tracked source files");

    WriteTextFile(ctx.cloneRepo / implementation, "// converge implementation\n// intent scoped commit planner\n");
    WriteTextFile(ctx.cloneRepo / regressionTest, "// converge tests\n// source intent regression\n");
    WriteTextFile(ctx.cloneRepo / repoConfig, "# repo catalog\n");
    WriteTextFile(ctx.cloneRepo / composeConfig, "services:\n  webview:\n    image: test\n");
    WriteTextFile(ctx.cloneRepo / workflowTemplate, "# feature notes\n");
    WriteTextFile(ctx.cloneRepo / powerShellCache, "PowerShell module cache\n");
    WriteTextFile(ctx.cloneRepo / shellLog, "local shell log\n");
    WriteTextFile(ctx.cloneRepo / nulDiff, "");
    WriteTextFile(ctx.cloneRepo / topicActive, "2026-07-10-platform-budget-wave\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[KOG-Converge][BugFix] Update intent-scoped agent commits (NO-TICKET)");
    RequireContains(result.stdoutText, "[KOG][Chore] Update configuration (NO-TICKET)");
    RequireContains(result.stdoutText, "[KOG][Chore] Update repository policy (NO-TICKET)");
    RequireContains(result.stdoutText, "[KOG][Docs] Update workflow templates (NO-TICKET)");
    RequireNotContains(result.stdoutText, "fix(kog-");
    RequireNotContains(result.stdoutText, "chore(kog");
    RequireNotContains(result.stdoutText, "docs(kog");
    RequireNotContains(result.stdoutText, "ambiguous " + powerShellCache.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + shellLog.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + nulDiff.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + topicActive.generic_string());
    RequireContains(result.stdoutText, "local " + powerShellCache.generic_string());
    RequireContains(result.stdoutText, "local " + shellLog.generic_string());
    RequireContains(result.stdoutText, "local " + nulDiff.generic_string());
    RequireContains(result.stdoutText, "local " + topicActive.generic_string());
    RequireContains(result.stdoutText, "ignore /Microsoft/Windows/PowerShell/ModuleAnalysisCache");
    RequireContains(result.stdoutText, "ignore /$log");
    RequireContains(result.stdoutText, "ignore /NUL_diff.txt");
    RequireContains(result.stdoutText, "ignore /topics/*/.active");
    RequireNotContains(result.stdoutText, "ambiguous docker-compose.webview.yml");
    RequireContains(result.stdoutText, "include .gitignore");
    RequireContains(result.stdoutText, "include " + implementation.generic_string());
    RequireContains(result.stdoutText, "include " + regressionTest.generic_string());
    RequireContains(result.stdoutText, "include " + composeConfig.generic_string());
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto log = RunGit({"log", "--format=%s", "-n", "5"}, ctx.cloneRepo);
    RequireSuccess(log, "read source intent commit log");
    RequireContains(log.stdoutText, "[KOG-Converge][BugFix] Update intent-scoped agent commits (NO-TICKET)");
    RequireContains(log.stdoutText, "[KOG][Chore] Update configuration (NO-TICKET)");
    RequireContains(log.stdoutText, "[KOG][Chore] Update repository policy (NO-TICKET)");
    RequireContains(log.stdoutText, "[KOG][Docs] Update workflow templates (NO-TICKET)");
    RequireNotContains(log.stdoutText, "fix(kog-");
    RequireNotContains(log.stdoutText, "chore(kog");
    RequireNotContains(log.stdoutText, "docs(kog");
    RequireNotContains(log.stdoutText, "update 2 files");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode keeps unknown local paths ambiguous", "[functional][converge][agent-mode][intent-commits][KG-BUG-0022]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-unknown-local-path");
    const auto unknownPath = std::filesystem::path("scratch/operator-owned.data");
    WriteTextFile(ctx.cloneRepo / unknownPath, "must remain operator-owned\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stdoutText, "ambiguous " + unknownPath.generic_string());
    RequireContains(result.stderrText, "no intent-scoped groups could be inferred");
    RequireContains(GitStatusShort(ctx.cloneRepo), "?? scratch/");
    REQUIRE(std::filesystem::exists(ctx.cloneRepo / unknownPath));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode scopes add delete and rename source paths to one ticket intent", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits][status-kind]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-ticket-status-intent");

    const auto implementation = std::filesystem::path("src/cpp/code/systems/kano_git_command/repo_sync/private/converge_cmd.cpp");
    const auto removedDoc = std::filesystem::path("docs/scoped-commit-obsolete.md");
    const auto renameOld = std::filesystem::path("docs/scoped-commit-old-note.md");
    const auto renameNew = std::filesystem::path("docs/KG-BUG-0005-scoped-commit-note.md");
    const auto regressionTest = std::filesystem::path("src/cpp/code/tests/kano_git_cli_tests/functional/KG-BUG-0005_status_kind_test.cpp");
    const auto evidence = std::filesystem::path("artifacts/KG-BUG-0005/status-kind-evidence.md");

    WriteTextFile(ctx.seedRepo / implementation, "// converge implementation\n");
    WriteTextFile(ctx.seedRepo / removedDoc, "# obsolete scoped commit note\n");
    WriteTextFile(ctx.seedRepo / renameOld, "# old scoped commit note\n");
    RequireSuccess(RunGit({"add", "src", "docs"}, ctx.seedRepo), "seed tracked source and docs for ticket context");
    RequireSuccess(RunGit({"commit", "-m", "seed ticket scoped source files"}, ctx.seedRepo), "commit ticket context seed");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push ticket context seed");
    RequireSuccess(RunGit({"pull", "--rebase", "origin", ctx.branch}, ctx.cloneRepo), "pull ticket context seed");

    WriteTextFile(ctx.cloneRepo / implementation, "// converge implementation\n// status-aware ticket context\n");
    std::filesystem::remove(ctx.cloneRepo / removedDoc);
    RequireSuccess(
        RunGit({"mv", renameOld.generic_string(), renameNew.generic_string()}, ctx.cloneRepo),
        "stage rename that converge must keep scoped after reset");
    WriteTextFile(ctx.cloneRepo / regressionTest, "// KG-BUG-0005 status-kind regression\n");
    WriteTextFile(ctx.cloneRepo / evidence, "# KG-BUG-0005 status-kind evidence\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[KOG-Converge][BugFix] Update KG-BUG-0005 implementation intent (KG-BUG-0005)");
    RequireContains(result.stdoutText, "include " + implementation.generic_string());
    RequireContains(result.stdoutText, "include " + removedDoc.generic_string());
    RequireContains(result.stdoutText, "include " + renameOld.generic_string());
    RequireContains(result.stdoutText, "include " + renameNew.generic_string());
    RequireContains(result.stdoutText, "include " + regressionTest.generic_string());
    RequireContains(result.stdoutText, "include " + evidence.generic_string());
    RequireNotContains(result.stdoutText, "[KOG-Converge][BugFix] Update intent-scoped agent commits (NO-TICKET)");
    RequireNotContains(result.stdoutText, "[KOG][Docs] Update documentation (NO-TICKET)");
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto log = RunGit({"log", "--format=%s", "-n", "3"}, ctx.cloneRepo);
    RequireSuccess(log, "read ticket context commit log");
    RequireContains(log.stdoutText, "[KOG-Converge][BugFix] Update KG-BUG-0005 implementation intent (KG-BUG-0005)");
    RequireNotContains(log.stdoutText, "[KOG][Docs] Update documentation (NO-TICKET)");
    RequireNotContains(log.stdoutText, "[KOG-Converge][BugFix] Update intent-scoped agent commits (NO-TICKET)");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge agent mode groups generic automation pipeline scripts", "[tdd][functional][feature:converge-state][converge][agent-mode][intent-commits]") {
    const auto ctx = CreateRemoteWithClone("converge-agent-automation-scripts");

    const auto validator = std::filesystem::path("bin/validate/orchestrator_contracts.py");
    const auto pipeline = std::filesystem::path("vars/unrealBuild.groovy");

    WriteTextFile(ctx.seedRepo / validator, "# validator\n");
    WriteTextFile(ctx.seedRepo / pipeline, "// pipeline\n");
    RequireSuccess(RunGit({"add", "bin", "vars"}, ctx.seedRepo), "seed tracked automation scripts");
    RequireSuccess(RunGit({"commit", "-m", "seed automation scripts"}, ctx.seedRepo), "commit automation script seed");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push automation script seed");
    RequireSuccess(RunGit({"pull", "--rebase", "origin", ctx.branch}, ctx.cloneRepo), "pull automation script seed");

    WriteTextFile(ctx.cloneRepo / validator, "# validator\n# scoped by automation classifier\n");
    WriteTextFile(ctx.cloneRepo / pipeline, "// pipeline\n// scoped by automation classifier\n");

    const auto result = RunKogWithEnv(
        {"converge", "--no-recursive", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KANO_AGENT_MODE", "1"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Converge agent intent commit plan");
    RequireContains(result.stdoutText, "[Automation][Chore] Update automation scripts (NO-TICKET)");
    RequireContains(result.stdoutText, "include " + validator.generic_string());
    RequireContains(result.stdoutText, "include " + pipeline.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + validator.generic_string());
    RequireNotContains(result.stdoutText, "ambiguous " + pipeline.generic_string());
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto log = RunGit({"log", "--format=%s", "-n", "3"}, ctx.cloneRepo);
    RequireSuccess(log, "read automation script intent commit log");
    RequireContains(log.stdoutText, "[Automation][Chore] Update automation scripts (NO-TICKET)");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime repeats passes until nested parent pointers are clean", "[tdd][functional][feature:converge-state][feature:dirty-kind][converge][planner]") {
    const auto ctx = CreateRemoteWithNestedSubmoduleClone("converge-runtime-nested-fixpoint");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneLeafRepo), "checkout leaf branch for nested fixpoint");
    WriteTextFile(ctx.cloneLeafRepo / "leaf.txt", "leaf seed\nnested runtime change\n");

    const auto result = RunKog({"converge", "--jobs", "1"}, ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "[converge] remaining actions detected; next pass required");
    RequireContains(result.stdoutText, "[converge] pass=2");
    RequireContains(result.stdoutText, "[converge] completed");
    REQUIRE(GitStatusShort(ctx.cloneLeafRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneMidRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneRootRepo).empty());

    const auto leafHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneLeafRepo);
    const auto leafOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneLeafRepo);
    const auto midHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneMidRepo);
    const auto midOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneMidRepo);
    const auto rootHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneRootRepo);
    const auto rootOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRootRepo);
    RequireSuccess(leafHead, "leaf rev-parse HEAD");
    RequireSuccess(leafOrigin, "leaf rev-parse origin");
    RequireSuccess(midHead, "mid rev-parse HEAD");
    RequireSuccess(midOrigin, "mid rev-parse origin");
    RequireSuccess(rootHead, "root rev-parse HEAD");
    RequireSuccess(rootOrigin, "root rev-parse origin");
    REQUIRE(leafHead.stdoutText == leafOrigin.stdoutText);
    REQUIRE(midHead.stdoutText == midOrigin.stdoutText);
    REQUIRE(rootHead.stdoutText == rootOrigin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner skips unregistered gitlink missing gitmodules mapping", "[functional][converge][planner][unregistered-gitlink]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-unregistered-gitlink");
    const auto child = (ctx.cloneRepo / "HorizonDialogueDemo").lexically_normal();
    InitPlainGitRepo(child);

    RequireSuccess(RunGit({"add", "HorizonDialogueDemo"}, ctx.cloneRepo), "stage unregistered gitlink");
    RequireSuccess(RunGit({"commit", "-m", "add unregistered gitlink"}, ctx.cloneRepo), "commit unregistered gitlink");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRepo), "push unregistered gitlink baseline");

    WriteTextFile(child / "README.md", "repo\nchild moved\n");
    RequireSuccess(RunGit({"commit", "-am", "child moved"}, child), "advance child gitlink commit");

    const auto result = RunConvergeDryRun(ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Skipped unregistered gitlinks");
    RequireContains(result.stdoutText, "HorizonDialogueDemo: no .gitmodules mapping; not registered as managed submodule; skipped parent pointer update");
    RequireContains(result.stdoutText, "UNREGISTERED_GITLINK_DIRTY_ONLY_SKIPPED");
    RequireNotContains(result.stdoutText, "KOG_PLAN_UNAUDITABLE");
    RequireNotContains(result.stdoutText, "deterministic pointer commit");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner blocks conflicted repo before mutation", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-conflict");
    RequireSuccess(RunGit({"checkout", "-b", "side"}, ctx.cloneRepo), "checkout side");
    WriteTextFile(ctx.cloneRepo / "README.md", "side\n");
    RequireSuccess(RunGit({"commit", "-am", "side change"}, ctx.cloneRepo), "commit side");
    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneRepo), "checkout main");
    WriteTextFile(ctx.cloneRepo / "README.md", "main\n");
    RequireSuccess(RunGit({"commit", "-am", "main change"}, ctx.cloneRepo), "commit main");
    const auto merge = RunGit({"merge", "side"}, ctx.cloneRepo);
    REQUIRE(merge.exitCode != 0);
    const auto conflictStatus = GitStatusShort(ctx.cloneRepo);

    const auto result = RunConvergeDryRun(ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stdoutText, "CONFLICTED");
    RequireContains(result.stdoutText, "CONFLICTED: resolve conflicts before converge mutation");
    RequireContains(result.stdoutText, "Blocked repos");
    REQUIRE(GitStatusShort(ctx.cloneRepo) == conflictStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner states untracked policy and commit-ai decision", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-untracked");
    WriteTextFile(ctx.cloneRepo / "new-file.txt", "new\n");
    const auto dirtyStatus = GitStatusShort(ctx.cloneRepo);

    const auto result = RunConvergeDryRun(ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "UNTRACKED_ONLY");
    RequireContains(result.stdoutText, "kog commit -ai --repos .");
    RequireContains(result.stdoutText, "untracked files included by git add -A policy");
    REQUIRE(GitStatusShort(ctx.cloneRepo) == dirtyStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner commits staged index changes", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-index-dirty");
    WriteTextFile(ctx.cloneRepo / "README.md", "seed\nstaged change\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.cloneRepo), "stage README change");
    const auto dirtyStatus = GitStatusShort(ctx.cloneRepo);

    const auto result = RunConvergeDryRun(ctx.cloneRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "INDEX_DIRTY");
    RequireContains(result.stdoutText, "kog commit -ai --repos .");
    RequireContains(result.stdoutText, "staged/index changes included by commit policy");
    REQUIRE(GitStatusShort(ctx.cloneRepo) == dirtyStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner skips clean nested preflight-only branch blockers", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-clean-nested-detached");
    RequireSuccess(RunGit({"checkout", "--detach", "HEAD"}, ctx.cloneChildRepo), "detach clean child");
    const auto rootStatus = GitStatusShort(ctx.cloneRootRepo);

    const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "DETACHED_HEAD");
    RequireContains(result.stdoutText, "preflight-only clean nested repo skipped: DETACHED_HEAD");
    RequireNotContains(result.stdoutText, "blocked by recursive status preflight");
    REQUIRE(GitStatusShort(ctx.cloneRootRepo) == rootStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner makes command policy skips explicit", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-sync", "false"}, ctx.cloneRootRepo), "set kog-sync policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-commit", "false"}, ctx.cloneRootRepo), "set kog-commit policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-push", "false"}, ctx.cloneRootRepo), "set kog-push policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-hygiene", "false"}, ctx.cloneRootRepo), "set kog-hygiene policy");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.cloneRootRepo), "add policy gitmodules");
    RequireSuccess(RunGit({"commit", "-m", "set command policy"}, ctx.cloneRootRepo), "commit policy gitmodules");

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for policy test");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child ahead\n");
    RequireSuccess(RunGit({"commit", "-am", "child ahead"}, ctx.cloneChildRepo), "commit child ahead");
    WriteTextFile(ctx.cloneChildRepo / "untracked.txt", "untracked\n");

    const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode != 0);
    RequireContains(result.stdoutText, "Command-policy decisions");
    RequireContains(result.stdoutText, "sync=false commit=false push=false hygiene=false");
    RequireContains(result.stdoutText, "DIRTY_WORKTREE: UNTRACKED_ONLY but commandPolicy.commit=false");

    RemoveSandboxWorkspace(ctx.sandbox);
}


TEST_CASE("KOG-BDD-CONVERGE-001 child push failure blocks only dependent parent", "[bdd][functional][feature:converge][scenario:KOG-BDD-CONVERGE-001][featured][converge][planner]") {
    const auto scenarioId = std::string{"KOG-BDD-CONVERGE-001"};
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-push-disabled");
    const auto productB = CreateRemoteWithClone("converge-planner-product-b");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "converge",
                                  "child repo push failure blocks only the dependent parent",
                                  "KOG-BDD-CONVERGE-001 child push failure blocks only dependent parent");
        recorder.SetFeatured(true)
            .SetDiagramType("flowchart")
            .AddTag("bdd")
            .AddTag("feature:converge")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("ProductA")
            .AddActor("Build/Base")
            .AddActor("ProductB")
            .Given("ProductA depends on Build/Base through a submodule pointer")
            .AndGiven("Build/Base has an unpublished commit while kog-push=false")
            .AndGiven("ProductB is an unrelated clean repository")
            .When("kog converge --dry-run plans ProductA")
            .Then("ProductA is blocked from committing a pointer to the unavailable Build/Base commit")
            .AndThen("ProductB can still produce an independent safe converge plan");

        RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-sync", "true"}, ctx.cloneRootRepo), "set kog-sync policy");
        RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-commit", "true"}, ctx.cloneRootRepo), "set kog-commit policy");
        RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-push", "false"}, ctx.cloneRootRepo), "set kog-push policy");
        RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-hygiene", "true"}, ctx.cloneRootRepo), "set kog-hygiene policy");
        RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.cloneRootRepo), "add push-disabled policy");
        RequireSuccess(RunGit({"commit", "-m", "set push disabled policy"}, ctx.cloneRootRepo), "commit push-disabled policy");
        RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRootRepo), "push push-disabled policy");
        RunDiscoverFull(ctx.cloneRootRepo);

        RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for push-disabled test");
        WriteTextFile(ctx.cloneChildRepo / "child.txt", "unpushed child\n");
        RequireSuccess(RunGit({"commit", "-am", "unpushed child"}, ctx.cloneChildRepo), "commit unpushed child");

        const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
        INFO(result.stdoutText);
        INFO(result.stderrText);
        REQUIRE(result.exitCode != 0);
        RequireContains(result.stdoutText, "sync=true commit=true push=false hygiene=true");
        RequireContains(result.stdoutText, "push skipped by commandPolicy.push=false");
        RequireContains(result.stdoutText, "Blocked repos");
        RequireContains(result.stdoutText, "parent pointer references commit from push-disabled repo that is not available remotely");
        RequireNotContains(result.stdoutText, "ProductB");

        const auto productBPlan = RunConvergeDryRun(productB.cloneRepo);
        INFO(productBPlan.stdoutText);
        INFO(productBPlan.stderrText);
        REQUIRE(productBPlan.exitCode == 0);
        RequireContains(productBPlan.stdoutText, "Converge Plan");
        RequireNotContains(productBPlan.stdoutText, "parent pointer references commit from push-disabled repo");
    }

    RequireScenarioMetadata(scenarioId, "converge", "flowchart");

    RemoveSandboxWorkspace(productB.sandbox);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner sync false policy skips behind-only repo", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-planner-sync-policy");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-sync", "false"}, ctx.cloneRootRepo), "set kog-sync false");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-commit", "true"}, ctx.cloneRootRepo), "set kog-commit true");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-push", "true"}, ctx.cloneRootRepo), "set kog-push true");
    RequireSuccess(RunGit({"config", "-f", ".gitmodules", "submodule." + ctx.submodulePath + ".kog-hygiene", "true"}, ctx.cloneRootRepo), "set kog-hygiene true");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.cloneRootRepo), "add sync policy");
    RequireSuccess(RunGit({"commit", "-m", "set sync policy"}, ctx.cloneRootRepo), "commit sync policy");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.cloneRootRepo), "push sync policy");
    RunDiscoverFull(ctx.cloneRootRepo);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for behind-only test");
    WriteTextFile(ctx.childSeedRepo / "child.txt", "remote child moved\n");
    RequireSuccess(RunGit({"commit", "-am", "remote child moved"}, ctx.childSeedRepo), "commit remote child move");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.childSeedRepo), "push remote child move");
    RequireSuccess(RunGit({"fetch", "origin"}, ctx.cloneChildRepo), "fetch child remote move");

    const auto result = RunConvergeDryRun(ctx.cloneRootRepo);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "BEHIND_ONLY");
    RequireContains(result.stdoutText, "sync=false commit=true push=true hygiene=true");
    RequireContains(result.stdoutText, "sync skipped by commandPolicy.sync=false");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner syncs dirty behind repo before pushing committed changes", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-dirty-behind-sync");

    CommitAndPushFile(ctx.seedRepo, ctx.branch, "remote.txt", "remote moved\n", "remote moved");
    RequireSuccess(RunGit({"fetch", "origin"}, ctx.cloneRepo), "fetch remote movement for dirty behind plan");
    WriteTextFile(ctx.cloneRepo / "local.txt", "local dirty change\n");

    const auto result = RunConvergeDryRun(ctx.cloneRepo, {"--jobs", "1"});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    RequireContains(result.stdoutText, "Phase commit actions");
    RequireContains(result.stdoutText, ".: kog commit -ai --repos .");
    RequireContains(result.stdoutText, "Phase sync actions");
    RequireContains(result.stdoutText, ".: kog sync origin-latest after commit before push");
    RequireContains(result.stdoutText, "Phase push actions");
    RequireContains(result.stdoutText, ".: kog push --repos . after commit");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime applies bounded sync git timeout", "[functional][converge][timeout][KG-BUG-0014]") {
    const auto ctx = CreateRemoteWithClone("converge-runtime-bounded-sync-timeout");
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);
    const auto diagLogPath = FunctionalProcessDiagLogPath(ctx.cloneRepo);

    CommitAndPushFile(ctx.seedRepo, ctx.branch, "remote.txt", "remote moved\n", "remote moved");
    RequireSuccess(RunGit({"fetch", "origin"}, ctx.cloneRepo), "fetch remote movement for dirty behind runtime sync");
    WriteTextFile(ctx.cloneRepo / "local.txt", "local dirty change\n");

    const auto result = RunKogWithEnv(
        {"converge", "--jobs", "1"},
        ctx.cloneRepo,
        {{"KOG_SHELL_TIMEOUT_MS", "0"},
         {"KOG_CONVERGE_SYNC_TIMEOUT_MS", "12345"}});
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(std::filesystem::exists(statePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto diagLogText = ReadTextFile(diagLogPath);
    RequireContains(diagLogText, "fetch origin --prune --tags --quiet");
    RequireContains(diagLogText, "env_KOG_SHELL_TIMEOUT_MS=12345");
    RequireContains(diagLogText, "env_KOG_SHELL_CAPTURE_TIMEOUT_MS=12345");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge planner default avoids full scan and opt-in blocks untrusted nested repo", "[tdd][unit][feature:converge-state][feature:dirty-kind][functional][converge][planner]") {
    const auto ctx = CreateRemoteWithClone("converge-planner-no-full-scan");
    const auto nested = (ctx.cloneRepo / "nested" / "untrusted").lexically_normal();
    InitPlainGitRepo(nested);

    const auto defaultPlan = RunConvergeDryRun(ctx.cloneRepo);
    INFO(defaultPlan.stdoutText);
    INFO(defaultPlan.stderrText);
    REQUIRE(defaultPlan.exitCode == 0);
    RequireContains(defaultPlan.stdoutText, "unregisteredScan=disabled");
    RequireNotContains(defaultPlan.stdoutText, "nested/untrusted");

    const auto scannedPlan = RunConvergeDryRun(ctx.cloneRepo, {"--unregistered-scan"});
    INFO(scannedPlan.stdoutText);
    INFO(scannedPlan.stderrText);
    REQUIRE(scannedPlan.exitCode != 0);
    RequireContains(scannedPlan.stdoutText, "unregisteredScan=bounded");
    RequireContains(scannedPlan.stdoutText, "Discovered unregistered nested Git repository that is not in the trusted workspace manifest. Register it as a submodule/subrepo, ignore it, or move it outside the workspace.");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime writes JSON state and supports status/abort", "[tdd][unit][feature:converge-state][functional][converge][state]") {
    const auto ctx = CreateRemoteWithClone("converge-runtime-state");
    const auto beforeStatus = GitStatusShort(ctx.cloneRepo);
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);
    const auto diagLogPath = FunctionalProcessDiagLogPath(ctx.cloneRepo);

    RequireSuccess(RunGit({"remote", "remove", "origin"}, ctx.cloneRepo), "remove origin to force converge failure");

    const auto convergeResult = RunKog({"converge", "--jobs", "1"}, ctx.cloneRepo);
    INFO(convergeResult.stdoutText);
    INFO(convergeResult.stderrText);
    REQUIRE(convergeResult.exitCode != 0);
    REQUIRE(std::filesystem::exists(statePath));

    const auto stateJsonText = ReadTextFile(statePath);
    RequireContains(stateJsonText, "\"schemaName\": \"kog.convergeWorkflowState\"");
    RequireContains(stateJsonText, "\"workflow\": \"converge\"");
    RequireContains(stateJsonText, "\"currentPhase\"");
    RequireContains(stateJsonText, "\"completedPhases\"");
    RequireContains(stateJsonText, "\"phaseResults\"");
    RequireContains(stateJsonText, "\"resumeCommand\"");
    RequireContains(stateJsonText, "post-failure recursive status baseline refresh skipped");
    RequireContains(stateJsonText, "kog status --recursive skipped after phase failure");

    const auto diagLogText = ReadTextFile(diagLogPath);
    REQUIRE(CountOccurrences(diagLogText, "[process-diag] argv=status --recursive --format json") == 1);

    const auto statusResult = RunKog({"converge", "--status"}, ctx.cloneRepo);
    INFO(statusResult.stdoutText);
    INFO(statusResult.stderrText);
    REQUIRE(statusResult.exitCode == 0);
    RequireContains(statusResult.stdoutText, "converge state file:");
    RequireContains(statusResult.stdoutText, "currentPhase=");
    RequireContains(statusResult.stdoutText, "repoGraphFingerprint=");
    RequireContains(statusResult.stdoutText, "resumePossible=");
    RequireContains(statusResult.stdoutText, "suggestedNextAction=");
    RequireContains(statusResult.stdoutText, "resumeCommand=kog converge --resume");

    const auto abortResult = RunKog({"converge", "--abort"}, ctx.cloneRepo);
    INFO(abortResult.stdoutText);
    INFO(abortResult.stderrText);
    REQUIRE(abortResult.exitCode == 0);
    REQUIRE_FALSE(std::filesystem::exists(statePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo) == beforeStatus);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge status prints none when no state exists", "[tdd][unit][feature:converge-state][functional][converge][state]") {
    const auto ctx = CreateRemoteWithClone("converge-runtime-status-none");
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);
    REQUIRE_FALSE(std::filesystem::exists(statePath));

    const auto statusResult = RunKog({"converge", "--status"}, ctx.cloneRepo);
    INFO(statusResult.stdoutText);
    INFO(statusResult.stderrText);
    REQUIRE(statusResult.exitCode == 0);
    RequireContains(statusResult.stdoutText, "converge state: none");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("KOG-BDD-CONVERGE-002 resume allows baseline drift on failed repos", "[bdd][functional][feature:converge][scenario:KOG-BDD-CONVERGE-002][featured][converge][state]") {
    const auto scenarioId = std::string{"KOG-BDD-CONVERGE-002"};
    const auto ctx = CreateRemoteWithClone("converge-runtime-resume-baseline");
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);
    {
        ScenarioRecorder recorder(scenarioId,
                                  "converge",
                                  "resume accepts repaired failed repo live-state drift",
                                  "KOG-BDD-CONVERGE-002 resume allows baseline drift on failed repos");
        recorder.SetFeatured(true)
            .SetDiagramType("state")
            .AddTag("bdd")
            .AddTag("feature:converge")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("a converge run fails and writes saved workflow state")
            .AndGiven("the failed repo is repaired without changing the repo graph topology")
            .AndGiven("the failed repo live branch drifts but still has upstream tracking before resume")
            .When("the developer runs kog converge --resume")
            .Then("resume succeeds instead of rejecting baseline drift on the failed repo")
            .AndThen("the saved converge state is removed after success");

        RequireSuccess(RunGit({"remote", "remove", "origin"}, ctx.cloneRepo), "remove origin to force initial failure");
        const auto firstRun = RunKog({"converge", "--jobs", "1"}, ctx.cloneRepo);
        INFO(firstRun.stdoutText);
        INFO(firstRun.stderrText);
        REQUIRE(firstRun.exitCode != 0);
        REQUIRE(std::filesystem::exists(statePath));

        RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.cloneRepo), "restore origin before resume baseline check");
        RequireSuccess(RunGit({"checkout", "-b", "resume-alt-branch"}, ctx.cloneRepo), "switch to alternate branch before resume");
        RequireSuccess(RunGit({"push", "-u", "origin", "resume-alt-branch"}, ctx.cloneRepo), "publish alternate branch before resume");

        const auto resumeRun = RunKog({"converge", "--resume", "--jobs", "1"}, ctx.cloneRepo);
        INFO(resumeRun.stdoutText);
        INFO(resumeRun.stderrText);
        REQUIRE(resumeRun.exitCode == 0);
        REQUIRE_FALSE(std::filesystem::exists(statePath));
    }

    RequireScenarioMetadata(scenarioId, "converge", "state");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge resume allows planned parent pointer baseline drift", "[tdd][functional][feature:converge-state][converge][state]") {
    const auto ctx = CreateRemoteWithSubmoduleClone("converge-runtime-resume-parent-pointer-drift");
    const auto statePath = ConvergeStatePath(ctx.cloneRootRepo);

    RequireSuccess(RunGit({"checkout", ctx.branch}, ctx.cloneChildRepo), "checkout child branch for resume pointer drift");
    WriteTextFile(ctx.cloneChildRepo / "child.txt", "child seed\nresume pointer drift\n");
    RequireSuccess(RunGit({"add", "child.txt"}, ctx.cloneChildRepo), "stage child drift");
    RequireSuccess(RunGit({"commit", "-m", "child local drift"}, ctx.cloneChildRepo), "commit child drift");
    REQUIRE_FALSE(GitStatusShort(ctx.cloneRootRepo).empty());

    const auto workspaceRoot = ctx.cloneRootRepo.generic_string();
    const std::string stateJson =
        "{\n"
        "  \"workflow\": \"converge\",\n"
        "  \"schemaName\": \"kog.convergeWorkflowState\",\n"
        "  \"schemaVersion\": 1,\n"
        "  \"workspaceRoot\": \"" + workspaceRoot + "\",\n"
        "  \"startedAt\": \"2026-01-01T00:00:00Z\",\n"
        "  \"currentPhase\": \"commit-local-changes-if-needed\",\n"
        "  \"recursive\": true,\n"
        "  \"dryRunRequested\": false,\n"
        "  \"completedPhases\": [\"status-preflight-plan\"],\n"
        "  \"blockedReason\": \"commit-local-changes-if-needed encountered failures\",\n"
        "  \"blockedRepos\": [\"" + ctx.submodulePath + "\"],\n"
        "  \"resumeCommand\": \"kog converge --resume\",\n"
        "  \"phaseResults\": {},\n"
        "  \"commandLinesUsed\": {},\n"
        "  \"convergePlan\": {\"sync\": [], \"commit\": [\".\"], \"push\": [\".\", \"" + ctx.submodulePath + "\"], \"blocked\": [], \"waves\": []},\n"
        "  \"repoGraphFingerprint\": \"\",\n"
        "  \"repoBaselines\": [\". branch=" + ctx.branch + " head=0000000000000000000000000000000000000000 remote=origin upstream=origin/" + ctx.branch + " dirtyKind=CLEAN ahead=0\"],\n"
        "  \"repoTaxonomy\": {\"type\": {}, \"managementPolicy\": {}},\n"
        "  \"commandPolicy\": {},\n"
        "  \"detectedConflictInfo\": \"\",\n"
        "  \"results\": {\"succeeded\": [\".\"], \"failed\": [\"" + ctx.submodulePath + "\"], \"blocked\": [], \"skipped\": [], \"pending\": []}\n"
        "}\n";
    WriteTextFile(statePath, stateJson);

    const auto resumeRun = RunKog({"converge", "--resume", "--jobs", "1"}, ctx.cloneRootRepo);
    INFO(resumeRun.stdoutText);
    INFO(resumeRun.stderrText);
    REQUIRE(resumeRun.exitCode == 0);
    RequireNotContains(resumeRun.stderrText, "successful repo baseline changed");
    REQUIRE_FALSE(std::filesystem::exists(statePath));
    REQUIRE(GitStatusShort(ctx.cloneChildRepo).empty());
    REQUIRE(GitStatusShort(ctx.cloneRootRepo).empty());

    const auto childHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneChildRepo);
    const auto childOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneChildRepo);
    const auto rootHead = RunGit({"rev-parse", "HEAD"}, ctx.cloneRootRepo);
    const auto rootOrigin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRootRepo);
    RequireSuccess(childHead, "resume pointer drift child rev-parse HEAD");
    RequireSuccess(childOrigin, "resume pointer drift child rev-parse origin");
    RequireSuccess(rootHead, "resume pointer drift root rev-parse HEAD");
    RequireSuccess(rootOrigin, "resume pointer drift root rev-parse origin");
    REQUIRE(childHead.stdoutText == childOrigin.stdoutText);
    REQUIRE(rootHead.stdoutText == rootOrigin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge runtime resume continues from saved phase", "[tdd][unit][feature:converge-state][functional][converge][state]") {
    const auto ctx = CreateRemoteWithClone("converge-runtime-resume");
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);

    RequireSuccess(RunGit({"remote", "remove", "origin"}, ctx.cloneRepo), "remove origin to force initial failure");
    const auto firstRun = RunKog({"converge", "--jobs", "1"}, ctx.cloneRepo);
    INFO(firstRun.stdoutText);
    INFO(firstRun.stderrText);
    REQUIRE(firstRun.exitCode != 0);
    REQUIRE(std::filesystem::exists(statePath));

    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.cloneRepo), "restore origin before resume");
    RequireSuccess(RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo), "fetch restored origin before resume");
    RequireSuccess(RunGit({"branch", "--set-upstream-to", "origin/" + ctx.branch, ctx.branch}, ctx.cloneRepo), "restore upstream before resume");
    const auto resumeRun = RunKog({"converge", "--resume", "--jobs", "1"}, ctx.cloneRepo);
    INFO(resumeRun.stdoutText);
    INFO(resumeRun.stderrText);
    REQUIRE(resumeRun.exitCode == 0);
    REQUIRE_FALSE(std::filesystem::exists(statePath));

    const auto statusResult = RunKog({"converge", "--status"}, ctx.cloneRepo);
    INFO(statusResult.stdoutText);
    INFO(statusResult.stderrText);
    REQUIRE(statusResult.exitCode == 0);
    RequireContains(statusResult.stdoutText, "converge state: none");

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("converge resume rewinds push to saved post-commit sync", "[functional][converge][state][resume][KG-BUG-0026]") {
    const auto ctx = CreateRemoteWithClone("converge-resume-saved-sync");
    const auto statePath = ConvergeStatePath(ctx.cloneRepo);

    WriteTextFile(ctx.seedRepo / "remote.txt", "remote change\n");
    RequireSuccess(RunGit({"add", "remote.txt"}, ctx.seedRepo), "stage remote resume change");
    RequireSuccess(RunGit({"commit", "-m", "remote resume change"}, ctx.seedRepo), "commit remote resume change");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "push remote resume change");

    WriteTextFile(ctx.cloneRepo / "local.txt", "local change\n");
    RequireSuccess(RunGit({"add", "local.txt"}, ctx.cloneRepo), "stage local resume change");
    RequireSuccess(RunGit({"commit", "-m", "local resume change"}, ctx.cloneRepo), "commit local resume change");
    RequireSuccess(RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo), "fetch divergent remote resume change");

    const auto workspaceRoot = ctx.cloneRepo.generic_string();
    const std::string stateJson =
        "{\n"
        "  \"workflow\": \"converge\",\n"
        "  \"schemaName\": \"kog.convergeWorkflowState\",\n"
        "  \"schemaVersion\": 1,\n"
        "  \"workspaceRoot\": \"" + workspaceRoot + "\",\n"
        "  \"startedAt\": \"2026-01-01T00:00:00Z\",\n"
        "  \"currentPhase\": \"push-nested-bottom-up\",\n"
        "  \"recursive\": false,\n"
        "  \"dryRunRequested\": false,\n"
        "  \"completedPhases\": [\"status-preflight-plan\", \"commit-local-changes-if-needed\", \"sync-before-push\"],\n"
        "  \"blockedReason\": \"push-nested-bottom-up encountered failures\",\n"
        "  \"blockedRepos\": [\".\"],\n"
        "  \"resumeCommand\": \"kog converge --resume --no-recursive\",\n"
        "  \"phaseResults\": {},\n"
        "  \"commandLinesUsed\": {},\n"
        "  \"convergePlan\": {\"sync\": [\".\"], \"commit\": [\".\"], \"push\": [\".\"], \"blocked\": [], \"waves\": []},\n"
        "  \"repoGraphFingerprint\": \"\",\n"
        "  \"repoBaselines\": [],\n"
        "  \"repoTaxonomy\": {\"type\": {}, \"managementPolicy\": {}},\n"
        "  \"commandPolicy\": {},\n"
        "  \"detectedConflictInfo\": \"\",\n"
        "  \"results\": {\"succeeded\": [\".\"], \"failed\": [\".\"], \"blocked\": [], \"skipped\": [], \"pending\": []}\n"
        "}\n";
    WriteTextFile(statePath, stateJson);

    const auto resumeRun = RunKog({"converge", "--resume", "--no-recursive", "--jobs", "1"}, ctx.cloneRepo);
    INFO(resumeRun.stdoutText);
    INFO(resumeRun.stderrText);
    REQUIRE(resumeRun.exitCode == 0);
    RequireContains(resumeRun.stdoutText, "resume_rewind=sync-before-push reason=saved_sync_repo_still_behind");
    RequireContains(resumeRun.stdoutText, "sync_repo=. ");
    RequireNotContains(resumeRun.stdoutText + resumeRun.stderrText, "non-fast-forward");
    REQUIRE_FALSE(std::filesystem::exists(statePath));
    REQUIRE(GitStatusShort(ctx.cloneRepo).empty());

    const auto head = RunGit({"rev-parse", "HEAD"}, ctx.cloneRepo);
    const auto origin = RunGit({"rev-parse", "origin/" + ctx.branch}, ctx.cloneRepo);
    RequireSuccess(head, "read resumed local head");
    RequireSuccess(origin, "read resumed origin head");
    REQUIRE(head.stdoutText == origin.stdoutText);

    RemoveSandboxWorkspace(ctx.sandbox);
}

} // namespace kano::git::tests::functional
