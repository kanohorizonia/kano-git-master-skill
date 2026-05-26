#include "bdd_scenario_recorder.hpp"
#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

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
    RequireSuccess(RunGit({"add", "README.md"}, InRepo), "add repo readme");
    RequireSuccess(RunGit({"commit", "-m", "seed repo"}, InRepo), "commit repo readme");
}

auto RunDiscoverJson(const std::filesystem::path& InRoot, const std::vector<std::string>& InExtraArgs) -> std::string {
    std::vector<std::string> args{"discover", "--format", "json", "--repo-root", InRoot.string(), "--no-cache"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    const auto result = RunKog(args, InRoot);
    RequireSuccess(result, "kog discover json");
    return StripAnsi(result.stdoutText + "\n" + result.stderrText);
}

auto RunDiscoverTable(const std::filesystem::path& InRoot, const std::vector<std::string>& InExtraArgs = {}) -> std::string {
    std::vector<std::string> args{"discover", "--format", "table", "--repo-root", InRoot.string(), "--no-cache"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    const auto result = RunKog(args, InRoot);
    RequireSuccess(result, "kog discover table");
    return StripAnsi(result.stdoutText + "\n" + result.stderrText);
}

auto JsonPathCount(const std::string& InJson, const std::string& InPathFragment) -> int {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = InJson.find(InPathFragment, pos)) != std::string::npos) {
        count += 1;
        pos += InPathFragment.size();
    }
    return count;
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

} // namespace

TEST_CASE("discover keeps registered recursion separate from bounded unregistered probing", "[tdd][unit][feature:discovery][functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-registered-recursion");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "registered");
    InitRepo(root / "registered" / "nested");
    InitRepo(root / "loose");
    InitRepo(root / "deep" / "too-far");

    AddGitmodulesEntry(root, "registered", "registered", "\tkog-sync = true\n\tkog-commit = true\n\tkog-push = false\n\tkog-hygiene = true\n");
    AddGitmodulesEntry(root / "registered", "nested", "nested", "\tkog-sync = true\n");
    CommitGitmodules(root / "registered");
    CommitGitmodules(root);

    const auto registeredOnly = RunDiscoverJson(root, {"--unregistered-depth", "0"});
    INFO(registeredOnly);
    REQUIRE(registeredOnly.find("registered") != std::string::npos);
    REQUIRE(registeredOnly.find("nested") != std::string::npos);
    REQUIRE(registeredOnly.find("\"kogPush\":\"false\"") != std::string::npos);
    REQUIRE(registeredOnly.find("loose") == std::string::npos);

    const auto bounded = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
    INFO(bounded);
    REQUIRE(bounded.find("loose") != std::string::npos);
    REQUIRE(bounded.find("too-far") != std::string::npos);
    REQUIRE(JsonPathCount(bounded, "registered") >= 1);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover no-unregistered-scan keeps trusted unregistered manifest entries", "[tdd][unit][feature:discovery][functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-no-unregistered-scan");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "registered");
    InitRepo(root / "loose");
    AddGitmodulesEntry(root, "registered", "registered", "\tkog-sync = true\n");
    CommitGitmodules(root);

    const auto full = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
    INFO(full);
    REQUIRE(full.find("loose") != std::string::npos);

    InitRepo(root / "new-loose");

    const auto noScan = RunDiscoverJson(root, {"--no-unregistered-scan"});
    INFO(noScan);
    REQUIRE(noScan.find("registered") != std::string::npos);
    REQUIRE(noScan.find("loose") != std::string::npos);
    REQUIRE(noScan.find("new-loose") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover treats trusted unregistered repo as registered discovery root", "[bdd][functional][feature:discovery][scenario:KOG-BDD-DISCOVERY-001][featured][discover][inventory]") {
    const auto scenarioId = std::string{"KOG-BDD-DISCOVERY-001"};
    const auto sandbox = CreateSandboxWorkspace("discover-producta-build-base-root");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "discovery",
                                  "trusted unregistered Build/Base root participates in registered recursion",
                                  "discover treats trusted unregistered repo as registered discovery root");
        recorder.SetFeatured(true)
            .SetDiagramType("graph")
            .AddTag("bdd")
            .AddTag("feature:discovery")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("ProductA contains an unregistered Build/Base Git repository")
            .AndGiven("Build/Base registers a child repository through its own .gitmodules file")
            .When("a full discover run first records Build/Base as trusted workspace inventory")
            .AndWhen("the developer runs discover again without full unregistered scanning")
            .Then("ProductA still includes the trusted Build/Base repository")
            .AndThen("Build/Base is traversed as a registered discovery root and emits exactly one child relationship");

        const auto root = (sandbox.root / "ProductA").lexically_normal();
        const auto buildBase = root / "Build" / "Base";
        InitRepo(root);
        InitRepo(buildBase);
        InitRepo(buildBase / "child");
        AddGitmodulesEntry(buildBase, "child", "child", "\tkog-sync = true\n");
        CommitGitmodules(buildBase);

        const auto full = RunDiscoverJson(root, {"--full", "--unregistered-depth", "2"});
        INFO(full);
        RequireContains(full, "Build/Base");

        const auto trusted = RunDiscoverJson(root, {});
        INFO(trusted);
        RequireContains(trusted, "ProductA");
        RequireContains(trusted, "Build/Base");
        RequireContains(trusted, "child");
        REQUIRE(JsonPathCount(trusted, "Build/Base") >= 1);
        REQUIRE(JsonPathCount(trusted, "Build/Base/child\",\"type\":\"registered\"") == 1);
        RequireNotContains(trusted, "too-far");
    }

    RequireScenarioMetadata(scenarioId, "discovery", "graph");
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover full scan prunes ignored build cache and temp directories", "[tdd][unit][feature:discovery][functional][discover][inventory]") {
    const auto sandbox = CreateSandboxWorkspace("discover-ignored-dirs");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / ".cache" / "cached-repo");
    InitRepo(root / "build" / "build-repo");
    InitRepo(root / "tmp" / "tmp-repo");
    InitRepo(root / "visible-repo");

    const auto json = RunDiscoverJson(root, {"--full", "--unregistered-depth", "3"});
    INFO(json);
    REQUIRE(json.find("visible-repo") != std::string::npos);
    REQUIRE(json.find("cached-repo") == std::string::npos);
    REQUIRE(json.find("build-repo") == std::string::npos);
    REQUIRE(json.find("tmp-repo") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover table keeps fixed-width readable columns", "[functional][discover][table][format]") {
    const auto sandbox = CreateSandboxWorkspace("discover-table-format");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "kano");
    InitRepo(root / "child-repo");
    AddGitmodulesEntry(root, "kano", "kano", "\tkog-sync = true\n");
    AddGitmodulesEntry(root, "child-repo", "child-repo", "\tkog-sync = true\n");
    CommitGitmodules(root);

    const auto table = RunDiscoverTable(root);
    INFO(table);
    RequireContains(table, "#");
    RequireContains(table, "REPO");
    RequireContains(table, "BRANCH");
    RequireContains(table, "GROUP: .");
    RequireNotContains(table, "1kano");
    RequireNotContains(table, "2child-repo");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover table truncates long repo names with separation", "[functional][discover][table][format]") {
    const auto sandbox = CreateSandboxWorkspace("discover-table-long-repo");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);
    InitRepo(root / "kano-filesystem-safe-ops-skill");
    AddGitmodulesEntry(root, "kano-filesystem-safe-ops-skill", "kano-filesystem-safe-ops-skill", "\tkog-sync = true\n");
    CommitGitmodules(root);

    const auto table = RunDiscoverTable(root);
    INFO(table);
    RequireContains(table, "kano-filesystem-safe-op...");
    RequireNotContains(table, "skillmain");

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("discover table aligns two digit index and missing branch placeholder", "[functional][discover][table][format]") {
    const auto sandbox = CreateSandboxWorkspace("discover-table-index-branch");
    const auto root = (sandbox.root / "root").lexically_normal();
    InitRepo(root);

    for (int i = 0; i < 10; ++i) {
        const auto name = std::string{"repo-"} + std::to_string(i);
        InitRepo(root / name);
        AddGitmodulesEntry(root, name, name, "\tkog-sync = true\n");
    }
    CommitGitmodules(root);

    // Force missing branch (detached HEAD) for one repo.
    RequireSuccess(RunGit({"checkout", "--detach"}, root / "repo-9"), "detach repo branch");

    const auto table = RunDiscoverTable(root);
    INFO(table);
    RequireContains(table, "10");
    RequireContains(table, "repo-9");
    RequireContains(table, "-");
    RequireNotContains(table, "10repo-9");

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
