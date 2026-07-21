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

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
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

auto CurrentBranchName(const std::filesystem::path& InRepo) -> std::string {
    const auto result = RunGit({"branch", "--show-current"}, InRepo);
    RequireSuccess(result, "read current branch");
    return result.stdoutText.substr(0, result.stdoutText.find_first_of("\r\n"));
}

auto InitRepo(const std::filesystem::path& InRepo) -> void {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", InRepo.string()}, InRepo.parent_path()), "init repo");
    ConfigureIdentity(InRepo);
    WriteTextFile(InRepo / "README.md", "repo\n");
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit readme");
}

auto AddGitmodulesEntry(const std::filesystem::path& InRepo,
                        const std::string& InName,
                        const std::string& InPath,
                        const std::string& InExtraPolicy = {}) -> void {
    std::ofstream out(InRepo / ".gitmodules", std::ios::binary | std::ios::app);
    REQUIRE(out.good());
    out << "[submodule \"" << InName << "\"]\n";
    out << "\tpath = " << InPath << "\n";
    out << "\turl = ./" << InPath << "\n";
    out << InExtraPolicy;
}

auto CommitGitmodules(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"add", ".gitmodules"}, InRepo), "add .gitmodules");
    RequireSuccess(RunGit({"commit", "-m", "configure submodules"}, InRepo), "commit .gitmodules");
}

auto RunDiscover(const std::filesystem::path& InRoot) -> void {
    const auto result = RunKog({"discover", "--repo-root", InRoot.string(), "--format", "json", "--full", "--unregistered-depth", "2", "--no-cache"}, InRoot);
    RequireSuccess(result, "kog discover full json");
}

auto RunRecursiveJson(const std::filesystem::path& InRoot, int InJobs) -> std::string {
    const auto result = RunKog({"status", "--recursive", "--json", "--repo-root", InRoot.string(), "--jobs", std::to_string(InJobs)}, InRoot);
    RequireSuccess(result, "kog status recursive json");
    return result.stdoutText;
}

auto RunRecursiveJsonWithDepth(const std::filesystem::path& InRoot, int InDepth) -> std::string {
    const auto result = RunKog({"status", "--recursive", "--json", "--repo-root", InRoot.string(), "--unregistered-depth", std::to_string(InDepth)}, InRoot);
    RequireSuccess(result, "kog status recursive json with depth");
    return result.stdoutText;
}

auto RunRecursiveJsonNoUnregisteredScan(const std::filesystem::path& InRoot) -> std::string {
    const auto result = RunKog({"status", "--recursive", "--json", "--repo-root", InRoot.string(), "--no-unregistered-scan"}, InRoot);
    RequireSuccess(result, "kog status recursive json no unregistered scan");
    return result.stdoutText;
}

auto RunRecursiveJsonNoFetchHealth(const std::filesystem::path& InRoot) -> std::string {
    const auto result = RunKog({"status", "--recursive", "--json", "--repo-root", InRoot.string(), "--no-unregistered-scan", "--no-fetch-health"}, InRoot);
    RequireSuccess(result, "kog status recursive json no fetch health");
    return result.stdoutText;
}

auto RunRecursiveSummaryNoFetchHealth(const std::filesystem::path& InRoot) -> CommandResult {
    return RunKog({"status", "--recursive", "--summary", "--repo-root", InRoot.string(), "--no-unregistered-scan", "--no-fetch-health"}, InRoot);
}

auto ExtractStatusJsonPayload(const std::string& InText) -> std::string {
    const auto start = InText.find("{\"schemaName\":\"kog.recursiveStatusSnapshot\"");
    REQUIRE(start != std::string::npos);
    const auto end = InText.rfind('}');
    REQUIRE(end != std::string::npos);
    REQUIRE(end >= start);
    return InText.substr(start, end - start + 1);
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

auto RequireRepoCommandPolicy(const std::string& InJson,
                              const std::string& InRepoId,
                              const std::string& InExpectedPolicy) -> void {
    const auto idNeedle = "\"id\":\"" + InRepoId + "\"";
    const auto idPos = InJson.find(idNeedle);
    INFO("repo id=" << InRepoId);
    INFO(InJson);
    REQUIRE(idPos != std::string::npos);
    const auto nextRepo = InJson.find("{\"id\":", idPos + idNeedle.size());
    const auto repoJson = nextRepo == std::string::npos ? InJson.substr(idPos) : InJson.substr(idPos, nextRepo - idPos);
    RequireContains(repoJson, InExpectedPolicy);
}

} // namespace

TEST_CASE("status recursive json emits Task 4A schema and deterministic scheduler order", "[bdd][functional][feature:status-policy][scenario:KOG-BDD-STATUS-001][featured][status][recursive]") {
    const auto scenarioId = std::string{"KOG-BDD-STATUS-001"};
    const auto sandbox = CreateSandboxWorkspace("status-recursive-schema");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "status-policy",
                                  "recursive status exposes structured command policies",
                                  "status recursive json emits Task 4A schema and deterministic scheduler order");
        recorder.SetFeatured(true)
            .SetDiagramType("flowchart")
            .AddTag("bdd")
            .AddTag("feature:status-policy")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("a workspace root with one registered repo and one trusted unregistered repo")
            .AndGiven("the registered repo defines kog sync, commit, push, and hygiene policy in .gitmodules")
            .When("the developer runs kog status --recursive --json with one and four scheduler jobs")
            .Then("both scheduler runs emit the same Task 4A recursive status schema")
            .AndThen("each relevant repo entry exposes commandPolicy sync, commit, push, hygiene, and source");

        const auto root = (sandbox.root / "root").lexically_normal();
        InitRepo(root);
        InitRepo(root / "registered");
        InitRepo(root / "loose");

        AddGitmodulesEntry(root, "registered", "registered", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
        CommitGitmodules(root);

        WriteTextFile(root / "registered" / "dirty.txt", "dirty\n");
        RunDiscover(root);

        const auto jsonJobs1 = ExtractStatusJsonPayload(RunRecursiveJson(root, 1));
        const auto jsonJobs4 = ExtractStatusJsonPayload(RunRecursiveJson(root, 4));
        INFO(jsonJobs1);
        REQUIRE(jsonJobs1 == jsonJobs4);

        RequireContains(jsonJobs1, "\"schemaName\":\"kog.recursiveStatusSnapshot\"");
        RequireContains(jsonJobs1, "\"schemaVersion\":1");
        RequireContains(jsonJobs1, "\"workspaceRoot\":");
        RequireContains(jsonJobs1, "\"repos\":[");
        RequireContains(jsonJobs1, "\"summary\":");

        for (const std::string field : {
                 "id", "type", "path", "absolutePath", "depth", "isWorkspaceRoot", "isContainerRoot",
                 "isSubmodule", "parentRepos", "childRepos", "branch", "head", "remote", "upstream",
                 "ahead", "behind", "dirtyKind", "statusFlags", "submoduleFacts", "conflicted", "pushable",
                 "selectedPushRemote", "registrationSource", "registrationRelativeTo", "isPersistedInWorkspaceManifest",
                 "containingRepo", "containingRelation", "isGitlinkInContainingRepo", "isIgnoredByContainingRepo",
                 "isExplicitlyAllowed", "managementPolicy", "blocksConverge", "blockReason", "commandPolicy", "diagnostics"}) {
            RequireContains(jsonJobs1, "\"" + field + "\"");
        }

        RequireContains(jsonJobs1, "\"type\":\"root\"");
        RequireContains(jsonJobs1, "\"type\":\"registered\"");
        RequireContains(jsonJobs1, "\"type\":\"unregistered\"");
        RequireContains(jsonJobs1, "\"dirtyKind\":\"UNTRACKED_ONLY\"");
        RequireRepoCommandPolicy(jsonJobs1, ".", "\"commandPolicy\":{\"sync\":true,\"commit\":true,\"push\":true,\"hygiene\":true,\"source\":\"workspace-root\"}");
        RequireRepoCommandPolicy(jsonJobs1, "registered", "\"commandPolicy\":{\"sync\":true,\"commit\":true,\"push\":false,\"hygiene\":true,\"source\":\"gitmodules\"}");
        RequireRepoCommandPolicy(jsonJobs1, "loose", "\"commandPolicy\":{\"sync\":true,\"commit\":true,\"push\":true,\"hygiene\":true,\"source\":\"workspace-manifest\"}");
        RequireNotContains(jsonJobs1, "\"kog-push\"");
        RequireNotContains(jsonJobs1, "\"kog-sync\"");
        RequireNotContains(jsonJobs1, "registered-uninit");
        RequireNotContains(jsonJobs1, "external-");
    }

    RequireScenarioMetadata(scenarioId, "status-policy", "flowchart");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive bounded scan reports newly discovered nested unregistered repo as blocking", "[tdd][unit][feature:status-policy][feature:dirty-kind][functional][status][recursive]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-untrusted");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "top-level-loose");
    InitRepo(root / "nested" / "untrusted");

    const auto json = ExtractStatusJsonPayload(RunRecursiveJsonWithDepth(root, 3));
    INFO(json);
    RequireContains(json, "\"id\":\"top-level-loose\"");
    RequireContains(json, "\"managementPolicy\":\"discovered-top-level\"");
    RequireContains(json, "\"id\":\"nested/untrusted\"");
    RequireContains(json, "\"type\":\"unregistered\"");
    RequireContains(json, "\"managementPolicy\":\"discovered-untrusted\"");
    RequireContains(json, "\"blocksConverge\":true");
    RequireContains(json, "\"blockReason\":\"Discovered unregistered nested Git repository that is not in the trusted workspace manifest. Register it as a submodule/subrepo, ignore it, or move it outside the workspace. If this is intended, run: kog discover --unregistered-depth 1\"");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive summary reports trusted unregistered repos without full scan", "[tdd][unit][feature:status-policy][feature:dirty-kind][functional][status][recursive]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-no-full-scan");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "trusted-loose");
    RunDiscover(root);
    InitRepo(root / "new-loose-after-manifest");

    const auto result = RunKog({"status", "--recursive", "--summary", "--repo-root", root.string(), "--no-unregistered-scan"}, root);
    RequireSuccess(result, "kog status recursive summary no full scan");
    INFO(result.stdoutText);
    RequireContains(result.stdoutText, "trusted-loose");
    RequireNotContains(result.stdoutText, "new-loose-after-manifest");
    RequireContains(result.stdoutText, "policy=manifest-trusted");

    const auto json = ExtractStatusJsonPayload(RunRecursiveJsonNoUnregisteredScan(root));
    RequireContains(json, "\"id\":\"trusted-loose\"");
    RequireNotContains(json, "\"id\":\"new-loose-after-manifest\"");
    RequireContains(json, "\"managementPolicy\":\"manifest-trusted\"");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive classifies snapshot deadline separately from repo status timeout", "[tdd][functional][status][recursive][timeout]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-snapshot-deadline");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    for (int i = 0; i < 6; ++i) {
        const auto childName = "child-" + std::to_string(i);
        InitRepo(root / childName);
        AddGitmodulesEntry(root, childName, childName);
    }
    CommitGitmodules(root);

    const auto result = RunKogWithEnv(
        {"status", "--recursive", "--json", "--repo-root", root.string(), "--jobs", "1", "--no-unregistered-scan", "--no-fetch-health"},
        root,
        {{"KOG_RECURSIVE_STATUS_DEADLINE_MS", "1"}});
    RequireSuccess(result, "kog status recursive json with forced snapshot deadline");
    const auto json = ExtractStatusJsonPayload(result.stdoutText);

    RequireContains(json, "STATUS_SNAPSHOT_DEADLINE");
    RequireContains(json, "recursive status snapshot deadline exceeded before this repo started preflight");
    RequireNotContains(json, "recursive status snapshot deadline exceeded before repo preflight");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive can skip remote fetch health checks", "[functional][status][recursive][fetch]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-no-fetch-health");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto bare = (sandbox.root / "origin.git").lexically_normal();
    InitRepo(root);
    const auto branch = CurrentBranchName(root);

    RequireSuccess(RunGit({"init", "--bare", bare.string()}, sandbox.root), "init bare origin");
    RequireSuccess(RunGit({"remote", "add", "origin", bare.string()}, root), "add origin");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, root), "push origin");
    RequireSuccess(RunGit({"remote", "add", "broken", "file:///missing/path/for/fetch"}, root), "add broken remote");

    const auto json = ExtractStatusJsonPayload(RunRecursiveJsonNoFetchHealth(root));
    INFO(json);
    RequireContains(json, "\"id\":\".\"");
    RequireNotContains(json, "FETCH_FAILED");
    RequireNotContains(json, "preflight FETCH_FAILED");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive summary does not count clean nested missing remote repos as blockers", "[functional][status][recursive][converge][KG-BUG-0013]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-clean-nested-missing-remote");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto bare = (sandbox.root / "origin.git").lexically_normal();
    InitRepo(root);
    InitRepo(root / "vendor");
    AddGitmodulesEntry(root, "vendor", "vendor");
    RequireSuccess(RunGit({"add", ".gitmodules", "vendor"}, root), "add vendor gitlink");
    RequireSuccess(RunGit({"commit", "-m", "configure vendor gitlink"}, root), "commit vendor gitlink");

    const auto branch = CurrentBranchName(root);
    RequireSuccess(RunGit({"init", "--bare", bare.string()}, sandbox.root), "init bare origin");
    RequireSuccess(RunGit({"remote", "add", "origin", bare.string()}, root), "add root origin");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, root), "push root origin");

    const auto summary = RunRecursiveSummaryNoFetchHealth(root);
    RequireSuccess(summary, "kog status recursive summary no fetch health");
    INFO(summary.stdoutText);
    RequireContains(summary.stdoutText, "repos=2 dirty=0 conflicted=0 blocksConverge=0");
    RequireContains(summary.stdoutText, "vendor type=registered dirtyKind=MISSING_REMOTE policy=managed");
    RequireNotContains(summary.stdoutText, "vendor type=registered dirtyKind=MISSING_REMOTE policy=managed blocksConverge=true");

    const auto json = ExtractStatusJsonPayload(RunRecursiveJsonNoFetchHealth(root));
    INFO(json);
    RequireContains(json, "\"dirtyCount\":0");
    RequireContains(json, "\"blocksConvergeCount\":0");
    RequireContains(json, "\"id\":\"vendor\"");
    RequireContains(json, "\"dirtyKind\":\"MISSING_REMOTE\"");
    RequireContains(json, "\"blocksConverge\":false");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive summary highlights conflicted repos", "[functional][status][recursive][conflict]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-conflict");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    const auto defaultBranch = CurrentBranchName(root);

    RequireSuccess(RunGit({"checkout", "-b", "feature/conflict"}, root), "checkout feature branch");
    WriteTextFile(root / "README.md", "feature change\n");
    RequireSuccess(RunGit({"add", "README.md"}, root), "add feature change");
    RequireSuccess(RunGit({"commit", "-m", "feature conflict change"}, root), "commit feature change");
    RequireSuccess(RunGit({"checkout", defaultBranch}, root), "checkout default branch");
    WriteTextFile(root / "README.md", "master change\n");
    RequireSuccess(RunGit({"add", "README.md"}, root), "add master change");
    RequireSuccess(RunGit({"commit", "-m", "master conflict change"}, root), "commit master change");

    const auto mergeResult = RunGit({"merge", "feature/conflict"}, root);
    INFO(mergeResult.stdoutText);
    INFO(mergeResult.stderrText);
    REQUIRE(mergeResult.exitCode != 0);

    const auto result = RunKog({"status", "--recursive", "--summary", "--repo-root", root.string(), "--no-unregistered-scan"}, root);
    RequireSuccess(result, "kog status recursive summary conflict highlight");
    INFO(result.stdoutText);
    RequireContains(result.stdoutText, "conflicted=1");
    RequireContains(result.stdoutText, "[CONFLICT] . type=root dirtyKind=CONFLICTED");
    RequireContains(result.stdoutText, "conflicted=true");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status positional repo target scopes discovery and reports ambiguous names deterministically", "[functional][status][repo-target][KG-TSK-0060]") {
    const auto sandbox = CreateSandboxWorkspace("status-positional-repo-target");
    const auto root = (sandbox.root / "root").lexically_normal();
    const auto serviceApi = (root / "services" / "api").lexically_normal();
    const auto servicePlugin = (serviceApi / "plugin").lexically_normal();
    const auto toolsApi = (root / "tools" / "api").lexically_normal();
    InitRepo(root);
    InitRepo(serviceApi);
    InitRepo(servicePlugin);
    InitRepo(toolsApi);
    AddGitmodulesEntry(serviceApi, "plugin", "plugin");
    CommitGitmodules(serviceApi);
    AddGitmodulesEntry(root, "service-api", "services/api");
    AddGitmodulesEntry(root, "tools-api", "tools/api");
    CommitGitmodules(root);

    const auto rootResult = RunKog(
        {"status", ".", "--all", "--no-cache", "--max-depth", "4", "--format", "json"},
        root);
    RequireSuccess(rootResult, "kog status dot target");
    RequireContains(rootResult.stdoutText, "\"repo_count\":4");
    RequireContains(rootResult.stdoutText, "\"path\":\"" + serviceApi.generic_string() + "\"");
    RequireContains(rootResult.stdoutText, "\"path\":\"" + servicePlugin.generic_string() + "\"");
    RequireContains(rootResult.stdoutText, "\"path\":\"" + toolsApi.generic_string() + "\"");

    const auto nestedResult = RunKog(
        {"status", "services/api", "--all", "--no-cache", "--max-depth", "4", "--format", "json"},
        root);
    RequireSuccess(nestedResult, "kog status nested repo target");
    RequireContains(nestedResult.stdoutText, "\"repo_count\":2");
    RequireContains(nestedResult.stdoutText, "\"repo_name\":\"api\"");
    RequireContains(nestedResult.stdoutText, "\"path\":\"" + servicePlugin.generic_string() + "\"");
    RequireNotContains(nestedResult.stdoutText, "tools/api");

    const auto singleRepoResult = RunKog(
        {"repo", "status", "services/api", "--repo-root", root.string(), "--format", "json"},
        root);
    RequireSuccess(singleRepoResult, "kog repo status nested repo target");
    RequireContains(singleRepoResult.stdoutText, "\"repo_count\":1");
    RequireContains(singleRepoResult.stdoutText, "\"repo_name\":\"api\"");
    RequireNotContains(singleRepoResult.stdoutText, "tools/api");

    const auto ambiguousResult = RunKog(
        {"status", "api", "--all", "--no-cache", "--max-depth", "4", "--format", "json"},
        root);
    const auto merged = ambiguousResult.stdoutText + "\n" + ambiguousResult.stderrText;
    INFO(merged);
    REQUIRE(ambiguousResult.exitCode != 0);
    RequireContains(merged, "repo spec is ambiguous: api");
    RequireContains(merged, "Matches:");
    RequireContains(merged, serviceApi.generic_string());
    RequireContains(merged, toolsApi.generic_string());
    REQUIRE(merged.find(serviceApi.generic_string()) < merged.find(toolsApi.generic_string()));

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
