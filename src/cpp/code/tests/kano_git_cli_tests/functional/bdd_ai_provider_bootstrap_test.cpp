#include "bdd_scenario_recorder.hpp"
#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

auto RequireNotContains(const std::string& InText, const std::string& InNeedle) -> void {
    INFO("unexpected needle=" << InNeedle);
    INFO(InText);
    REQUIRE(InText.find(InNeedle) == std::string::npos);
}

struct AiBootstrapSandbox {
    SandboxContext sandbox;
    std::filesystem::path home;
    std::filesystem::path userProfile;
    std::filesystem::path appData;
    std::filesystem::path localAppData;
    std::filesystem::path pathBin;
    std::filesystem::path metadataDir;
    std::filesystem::path diagLog;
    std::vector<std::pair<std::string, std::string>> env;
};

auto CreateAiBootstrapSandbox(const std::string& InName,
                              const std::string& InAvailableCommands) -> AiBootstrapSandbox {
    AiBootstrapSandbox out{.sandbox = CreateSandboxWorkspace(InName)};
    const auto scenarioEvidenceRoot = [&]() {
        if (const char* metadataRaw = std::getenv("KANO_BDD_METADATA_DIR"); metadataRaw != nullptr && metadataRaw[0] != '\0') {
            return std::filesystem::absolute(std::filesystem::path(metadataRaw)).lexically_normal();
        }
        return (out.sandbox.root / ".kano" / "tmp" / "bdd-metadata").lexically_normal();
    }();
    out.home = out.sandbox.root / "home";
    out.userProfile = out.sandbox.root / "userprofile";
    out.appData = out.sandbox.root / "appdata";
    out.localAppData = out.sandbox.root / "localappdata";
    out.pathBin = out.sandbox.root / "path-bin";
    out.metadataDir = scenarioEvidenceRoot;
    out.diagLog = out.metadataDir.parent_path() / (InName + "-functional-process-diag.log");
    std::filesystem::create_directories(out.home);
    std::filesystem::create_directories(out.userProfile);
    std::filesystem::create_directories(out.appData);
    std::filesystem::create_directories(out.localAppData);
    std::filesystem::create_directories(out.pathBin);
    std::filesystem::create_directories(out.metadataDir);
    std::filesystem::create_directories(out.diagLog.parent_path());

    out.env = {
        {"PATH", out.pathBin.string()},
        {"HOME", out.home.string()},
        {"USERPROFILE", out.userProfile.string()},
        {"APPDATA", out.appData.string()},
        {"LOCALAPPDATA", out.localAppData.string()},
        {"KOG_TEST_MODE", "1"},
        {"KOG_TEST_AVAILABLE_COMMANDS", InAvailableCommands},
        {"KANO_BDD_METADATA_DIR", out.metadataDir.string()},
        {"KANO_TEST_BINARY_NAME", "kano_git_cli_tests"},
        {"KOG_PROCESS_DIAGNOSTICS_LOG", out.diagLog.string()},
        {"KOG_SHELL_CAPTURE_TIMEOUT_MS", "15000"},
    };
    return out;
}

auto ReadDiagnosticsIfPresent(const AiBootstrapSandbox& InSandbox) -> std::string {
    if (!std::filesystem::exists(InSandbox.diagLog)) {
        return {};
    }
    return ReadTextFile(InSandbox.diagLog);
}

auto RequireHermeticProfileDirs(const AiBootstrapSandbox& InSandbox, const std::string& InDiagnostics) -> void {
    RequireContains(InDiagnostics, "selected_PATH=" + InSandbox.pathBin.string());
    RequireNotContains(InDiagnostics, "GitHub.Copilot --accept-source-agreements");
    RequireNotContains(InDiagnostics, "gh auth");
    RequireNotContains(InDiagnostics, "gh copilot");
    RequireNotContains(InDiagnostics, "npm");
    RequireNotContains(InDiagnostics, "copilot -s");
}

auto RequireScenarioMetadata(const AiBootstrapSandbox& InSandbox, const std::string& InScenarioId) -> void {
    const auto sidecar = InSandbox.metadataDir / (InScenarioId + ".json");
    REQUIRE(std::filesystem::exists(sidecar));
    const auto json = ReadTextFile(sidecar);
    RequireContains(json, "\"feature\": \"ai-provider-bootstrap\"");
    RequireContains(json, "\"scenarioId\": \"" + InScenarioId + "\"");
    RequireContains(json, "\"featured\": true");
    RequireContains(json, "\"diagramType\": \"sequence\"");
    RequireContains(json, "\"bdd\"");
    RequireContains(json, "\"feature:ai-provider-bootstrap\"");
    RequireContains(json, "\"scenario:" + InScenarioId + "\"");
    RequireContains(json, "\"featured\"");
}

} // namespace

TEST_CASE("KOG-BDD-AI-001 missing Copilot with WinGet points to bootstrap without installing",
          "[bdd][functional][feature:ai-provider-bootstrap][scenario:KOG-BDD-AI-001][featured]") {
    const auto scenarioId = std::string{"KOG-BDD-AI-001"};
    auto sandbox = CreateAiBootstrapSandbox("bdd-ai-001", "winget");
    const ScopedEnvVar metadataEnv("KANO_BDD_METADATA_DIR", sandbox.metadataDir.string());
    const ScopedEnvVar binaryEnv("KANO_TEST_BINARY_NAME", "kano_git_cli_tests");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "ai-provider-bootstrap",
                                  "missing Copilot with WinGet recommends kog bootstrap",
                                  "KOG-BDD-AI-001 missing Copilot with WinGet points to bootstrap without installing");
        recorder.SetFeatured(true)
            .SetDiagramType("sequence")
            .AddTag("bdd")
            .AddTag("feature:ai-provider-bootstrap")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("Copilot CLI is absent from a sandbox-owned PATH, APPDATA, and LOCALAPPDATA")
            .AndGiven("WinGet availability is declared only through KOG_TEST_MODE command probes")
            .When("the developer runs kog commit --ai-auto with native preflight skipped")
            .Then("kog refuses AI mode without installing software")
            .AndThen("the output points to kog ai bootstrap copilot");

        const auto result = RunKogWithEnv({"commit", "--ai-auto", "--no-native-preflight"}, sandbox.sandbox.root, sandbox.env);
        const auto merged = result.stdoutText + "\n" + result.stderrText;
        INFO(merged);
        REQUIRE(result.exitCode == 2);
        RequireContains(merged, "Copilot CLI is not installed.");
        RequireContains(merged, "Recommended Windows install:");
        RequireContains(merged, "winget install GitHub.Copilot");
        RequireContains(merged, "kog ai bootstrap copilot");
        RequireContains(merged, "Error: AI mode requested, but provider is unavailable.");
        RequireNotContains(merged, "--accept-source-agreements");
        RequireNotContains(merged, "npm install");
        RequireNotContains(merged, "npm i");
    }

    const auto diagnostics = ReadDiagnosticsIfPresent(sandbox);
    RequireHermeticProfileDirs(sandbox, diagnostics);
    RequireScenarioMetadata(sandbox, scenarioId);
    RemoveSandboxWorkspace(sandbox.sandbox);
}

TEST_CASE("KOG-BDD-AI-002 missing Copilot without WinGet explains App Installer and avoids npm-first guidance",
          "[bdd][functional][feature:ai-provider-bootstrap][scenario:KOG-BDD-AI-002][featured]") {
    const auto scenarioId = std::string{"KOG-BDD-AI-002"};
    auto sandbox = CreateAiBootstrapSandbox("bdd-ai-002", "");
    const ScopedEnvVar metadataEnv("KANO_BDD_METADATA_DIR", sandbox.metadataDir.string());
    const ScopedEnvVar binaryEnv("KANO_TEST_BINARY_NAME", "kano_git_cli_tests");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "ai-provider-bootstrap",
                                  "missing Copilot without WinGet explains Windows Package Manager repair",
                                  "KOG-BDD-AI-002 missing Copilot without WinGet explains App Installer and avoids npm-first guidance");
        recorder.SetFeatured(true)
            .SetDiagramType("sequence")
            .AddTag("bdd")
            .AddTag("feature:ai-provider-bootstrap")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("Copilot CLI is absent from sandbox-owned profile directories")
            .AndGiven("WinGet is unavailable in KOG_TEST_MODE command probes")
            .When("the developer runs kog commit --ai-auto with native preflight skipped")
            .Then("kog refuses AI mode without installing software")
            .AndThen("the output explains App Installer / Windows Package Manager repair")
            .AndThen("npm is not presented as the primary Windows install path");

        const auto result = RunKogWithEnv({"commit", "--ai-auto", "--no-native-preflight"}, sandbox.sandbox.root, sandbox.env);
        const auto merged = result.stdoutText + "\n" + result.stderrText;
        INFO(merged);
        REQUIRE(result.exitCode == 2);
        RequireContains(merged, "Copilot CLI is not installed and WinGet is unavailable.");
        RequireContains(merged, "Windows Package Manager / App Installer");
        RequireContains(merged, "Install or repair App Installer / WinGet");
        RequireContains(merged, "winget install GitHub.Copilot");
        RequireNotContains(merged, "kog ai bootstrap copilot");
        RequireNotContains(merged, "npm install");
        RequireNotContains(merged, "npm i");
    }

    const auto diagnostics = ReadDiagnosticsIfPresent(sandbox);
    RequireHermeticProfileDirs(sandbox, diagnostics);
    RequireScenarioMetadata(sandbox, scenarioId);
    RemoveSandboxWorkspace(sandbox.sandbox);
}

TEST_CASE("KOG-BDD-AI-003 bootstrap copilot dry-run previews WinGet without requiring Copilot",
          "[bdd][functional][feature:ai-provider-bootstrap][scenario:KOG-BDD-AI-003][featured]") {
    const auto scenarioId = std::string{"KOG-BDD-AI-003"};
    auto sandbox = CreateAiBootstrapSandbox("bdd-ai-003", "winget");
    const ScopedEnvVar metadataEnv("KANO_BDD_METADATA_DIR", sandbox.metadataDir.string());
    const ScopedEnvVar binaryEnv("KANO_TEST_BINARY_NAME", "kano_git_cli_tests");
    {
        ScenarioRecorder recorder(scenarioId,
                                  "ai-provider-bootstrap",
                                  "bootstrap dry-run previews WinGet install without side effects",
                                  "KOG-BDD-AI-003 bootstrap copilot dry-run previews WinGet without requiring Copilot");
        recorder.SetFeatured(true)
            .SetDiagramType("sequence")
            .AddTag("bdd")
            .AddTag("feature:ai-provider-bootstrap")
            .AddTag("scenario:" + scenarioId)
            .AddTag("featured")
            .AddActor("developer")
            .AddActor("kog")
            .Given("Copilot CLI is absent from sandbox-owned PATH and profile directories")
            .AndGiven("WinGet availability is declared only through KOG_TEST_MODE command probes")
            .When("the developer runs kog ai bootstrap copilot --dry-run")
            .Then("kog previews the WinGet install command")
            .AndThen("no real install is executed")
            .AndThen("copilot.exe is not required for the dry-run");

        const auto result = RunKogWithEnv({"ai", "bootstrap", "copilot", "--dry-run"}, sandbox.sandbox.root, sandbox.env);
        const auto merged = result.stdoutText + "\n" + result.stderrText;
        INFO(merged);
        REQUIRE(result.exitCode == 0);
        RequireContains(merged, "[dry-run] winget install GitHub.Copilot --accept-source-agreements --accept-package-agreements");
        RequireContains(merged, "No install was executed");
        RequireContains(merged, "copilot.exe is not required");
        RequireNotContains(merged, "Copilot bootstrap completed.");
        RequireNotContains(merged, "copilot -s --model auto");
        RequireNotContains(merged, "npm install");
    }

    const auto diagnostics = ReadDiagnosticsIfPresent(sandbox);
    RequireContains(diagnostics, "executable=");
    RequireContains(diagnostics, "argv=ai bootstrap copilot --dry-run");
    RequireHermeticProfileDirs(sandbox, diagnostics);
    RequireScenarioMetadata(sandbox, scenarioId);
    RemoveSandboxWorkspace(sandbox.sandbox);
}

} // namespace kano::git::tests::functional
