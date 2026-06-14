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

auto RunConvergeDryRun(const std::filesystem::path& InRoot, std::vector<std::string> InExtraArgs = {}) -> CommandResult {
    std::vector<std::string> args{"converge", "--dry-run"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKog(args, InRoot);
}

auto PlanPayload(const std::string& InText) -> std::string {
    const auto start = InText.find("Converge Plan");
    REQUIRE(start != std::string::npos);
    return InText.substr(start);
}

auto RunDiscoverFull(const std::filesystem::path& InRoot, int InDepth = 3) -> void {
    const auto result = RunKog({"discover", "--full", "--unregistered-depth", std::to_string(InDepth), "--format", "json", "--repo-root", InRoot.string(), "--no-cache"}, InRoot);
    RequireSuccess(result, "kog discover full json");
}

} // namespace

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

} // namespace kano::git::tests::functional
