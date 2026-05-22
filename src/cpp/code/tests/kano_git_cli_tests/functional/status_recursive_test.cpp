#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
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

} // namespace

TEST_CASE("status recursive json emits Task 4A schema and deterministic scheduler order", "[functional][status][recursive]") {
    const auto sandbox = CreateSandboxWorkspace("status-recursive-schema");
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
    RequireContains(jsonJobs1, "\"commandPolicy\":{\"sync\":true,\"commit\":true,\"push\":false,\"hygiene\":true,\"source\":\"gitmodules\"}");
    RequireNotContains(jsonJobs1, "\"kog-push\"");
    RequireNotContains(jsonJobs1, "\"kog-sync\"");
    RequireNotContains(jsonJobs1, "registered-uninit");
    RequireNotContains(jsonJobs1, "external-");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("status recursive bounded scan reports newly discovered nested unregistered repo as blocking", "[functional][status][recursive]") {
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

TEST_CASE("status recursive summary reports trusted unregistered repos without full scan", "[functional][status][recursive]") {
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

} // namespace kano::git::tests::functional
