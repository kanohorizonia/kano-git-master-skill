#include "bdd_scenario_recorder.hpp"
#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace kano::git::tests::functional {
namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string InName, std::string InValue)
        : name(std::move(InName)) {
        if (const char* raw = std::getenv(name.c_str()); raw != nullptr) {
            hadValue = true;
            previous = raw;
        }
        SetEnv(name, InValue);
    }

    ~ScopedEnvVar() {
        if (hadValue) {
            SetEnv(name, previous);
        } else {
            UnsetEnv(name);
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    auto operator=(const ScopedEnvVar&) -> ScopedEnvVar& = delete;

private:
    static auto SetEnv(const std::string& InName, const std::string& InValue) -> void {
#if defined(_WIN32)
        _putenv_s(InName.c_str(), InValue.c_str());
#else
        setenv(InName.c_str(), InValue.c_str(), 1);
#endif
    }

    static auto UnsetEnv(const std::string& InName) -> void {
#if defined(_WIN32)
        _putenv_s(InName.c_str(), "");
#else
        unsetenv(InName.c_str());
#endif
    }

    std::string name;
    bool hadValue = false;
    std::string previous;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& InPath)
        : previous(std::filesystem::current_path()) {
        std::filesystem::create_directories(InPath);
        std::filesystem::current_path(InPath);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(previous);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    auto operator=(const ScopedCurrentPath&) -> ScopedCurrentPath& = delete;

private:
    std::filesystem::path previous;
};

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto RequireContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("missing needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) != std::string::npos);
}

} // namespace

TEST_CASE("ScenarioRecorder writes Phase A BDD JSON to explicit metadata dir", "[functional][bdd][recorder]") {
    const auto sandbox = CreateSandboxWorkspace("bdd-recorder-explicit");
    const auto metadataDir = (sandbox.root / ".kano" / "tmp" / "bdd-explicit").lexically_normal();
    const ScopedEnvVar metadataEnv("KANO_BDD_METADATA_DIR", metadataDir.string());
    const ScopedEnvVar binaryEnv("KANO_TEST_BINARY_NAME", "kano_git_cli_tests_explicit.exe");

    {
        ScenarioRecorder recorder("KOG-BDD-RECORDER-001", "bdd-reporting", "records ordered BDD steps", "ScenarioRecorder explicit metadata smoke");
        recorder.SetFeatured(true)
            .SetLayer("functional")
            .SetDocVisibility("public")
            .SetAutomationStatus("automated")
            .SetDiagramType("sequence")
            .AddTag("bdd")
            .AddTag("scenario:KOG-BDD-RECORDER-001")
            .AddActor("developer")
            .AddTrace("TASK-5")
            .Given("an explicit BDD metadata directory")
            .AndGiven("a deterministic scenario id")
            .When("the recorder leaves scope")
            .AndWhen("the sidecar is flushed")
            .Then("a JSON sidecar is written")
            .AndThen("ordered steps are preserved");
    }

    const auto sidecar = metadataDir / "KOG-BDD-RECORDER-001.json";
    REQUIRE(std::filesystem::exists(sidecar));
    REQUIRE_FALSE(std::filesystem::exists(sidecar.string() + ".tmp"));
    const auto json = ReadTextFile(sidecar);
    RequireContains(json, "\"style\": \"bdd\"");
    RequireContains(json, "\"feature\": \"bdd-reporting\"");
    RequireContains(json, "\"scenarioId\": \"KOG-BDD-RECORDER-001\"");
    RequireContains(json, "\"scenarioTitle\": \"records ordered BDD steps\"");
    RequireContains(json, "\"featured\": true");
    RequireContains(json, "\"docVisibility\": \"public\"");
    RequireContains(json, "\"automationStatus\": \"automated\"");
    RequireContains(json, "\"diagramType\": \"sequence\"");
    RequireContains(json, "\"sourceTestName\": \"ScenarioRecorder explicit metadata smoke\"");
    RequireContains(json, "\"sourceTestBinary\": \"kano_git_cli_tests_explicit.exe\"");
    RequireContains(json, "\"relatedArtifacts\": []");
    RequireContains(json, "\"environment\": {");
    RequireContains(json, "\"lane\": \"functional\"");
    RequireContains(json, "\"project\": \"kano-git\"");
    RequireContains(json, "\"domain\": \"git\"");
    RequireContains(json, "\"actors\": [\"developer\"]");
    RequireContains(json, "\"traces\": [\"TASK-5\"]");

    const auto givenPos = json.find("an explicit BDD metadata directory");
    const auto andGivenPos = json.find("a deterministic scenario id");
    const auto whenPos = json.find("the recorder leaves scope");
    const auto andWhenPos = json.find("the sidecar is flushed");
    const auto thenPos = json.find("a JSON sidecar is written");
    const auto andThenPos = json.find("ordered steps are preserved");
    REQUIRE(givenPos < andGivenPos);
    REQUIRE(andGivenPos < whenPos);
    REQUIRE(whenPos < andWhenPos);
    REQUIRE(andWhenPos < thenPos);
    REQUIRE(thenPos < andThenPos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("ScenarioRecorder falls back to cwd .kano tmp BDD metadata path", "[functional][bdd][recorder]") {
    const auto sandbox = CreateSandboxWorkspace("bdd-recorder-fallback");
    const auto cwd = (sandbox.root / "cwd").lexically_normal();
    std::filesystem::path sidecar;

    {
        const ScopedEnvVar metadataEnv("KANO_BDD_METADATA_DIR", "");
        const ScopedCurrentPath scopedCwd(cwd);
        ScenarioRecorder recorder("KOG BDD Recorder 002", "bdd-reporting", "uses fallback path", "ScenarioRecorder fallback smoke");
        recorder.Given("no explicit metadata dir")
            .When("the recorder writes metadata")
            .Then("the cwd fallback receives the sidecar");
        sidecar = cwd / ".kano" / "tmp" / "test-metadata" / "bdd" / "KOG_BDD_Recorder_002.json";
    }

    REQUIRE(std::filesystem::exists(sidecar));
    const auto json = ReadTextFile(sidecar);
    RequireContains(json, "\"layer\": \"functional\"");
    RequireContains(json, "\"featured\": false");
    RequireContains(json, "\"docVisibility\": \"internal\"");
    RequireContains(json, "\"automationStatus\": \"automated\"");
    RequireContains(json, "\"diagramType\": \"flowchart\"");
    RequireContains(json, "\"scenarioId\": \"KOG BDD Recorder 002\"");

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
