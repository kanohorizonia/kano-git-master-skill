#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "secret_scan_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace kano::git::commands;

namespace {

void SetEnvVarForTest(const char* InName, const char* InValue) {
#if defined(_WIN32)
    _putenv_s(InName, InValue == nullptr ? "" : InValue);
#else
    if (InValue == nullptr || std::string(InValue).empty()) {
        unsetenv(InName);
    } else {
        setenv(InName, InValue, 1);
    }
#endif
}

struct CoutCapture {
    std::ostringstream stream;
    std::streambuf* previous = nullptr;

    CoutCapture() {
        previous = std::cout.rdbuf(stream.rdbuf());
    }

    ~CoutCapture() {
        std::cout.rdbuf(previous);
    }

    auto str() const -> std::string {
        return stream.str();
    }
};

} // namespace

TEST_CASE("FormatCommandLineForLog redacts obvious secrets", "[Unit][CommitPlan][Diagnostics]") {
    const auto formatted = FormatCommandLineForLog(
        "copilot",
        {"--token", "abc123", "--api-key=secret-value", "--password", "hunter2", "--authorization", "Bearer deadbeef"});

    REQUIRE(formatted.find("abc123") == std::string::npos);
    REQUIRE(formatted.find("secret-value") == std::string::npos);
    REQUIRE(formatted.find("hunter2") == std::string::npos);
    REQUIRE(formatted.find("deadbeef") == std::string::npos);
    REQUIRE(formatted.find("<redacted>") != std::string::npos);
}

TEST_CASE("PrintAiInvocationDiagnostics includes effective model and file locations", "[Unit][CommitPlan][Diagnostics]") {
    CoutCapture capture;
    AiInvocationDiagnostics diagnostics;
    diagnostics.purpose = "plan-fill";
    diagnostics.requestedProvider = "copilot";
    diagnostics.resolvedProvider = "copilot";
    diagnostics.requestedModel = "gpt-5.4";
    diagnostics.effectiveModel = "gpt-5.4";
    diagnostics.modelMode = "provider-default";
    diagnostics.yolo = true;
    diagnostics.promptFile = std::filesystem::path("C:/tmp/plan prompt.txt");
    diagnostics.workingFile = std::filesystem::path("C:/tmp/working-plan.json");
    diagnostics.responseFile = std::filesystem::path("C:/tmp/response.json");
    diagnostics.timeout = std::chrono::seconds{45};

    PrintAiInvocationDiagnostics(
        "copilot",
        {"--token", "abc123", "--model", "gpt-5.4"},
        diagnostics);

    const auto output = capture.str();
    REQUIRE(output.find("[kog ai] -- AI Invocation (plan-fill) --") != std::string::npos);
    REQUIRE(output.find("requested provider : copilot") != std::string::npos);
    REQUIRE(output.find("effective model    : gpt-5.4") != std::string::npos);
    REQUIRE(output.find("prompt dir") != std::string::npos);
    REQUIRE(output.find("response dir") != std::string::npos);
    REQUIRE(output.find("abc123") == std::string::npos);
    REQUIRE(output.find("<redacted>") != std::string::npos);
}

TEST_CASE("ExecuteCommandWithHeartbeat emits heartbeat while waiting", "[Unit][CommitPlan][Diagnostics][windows]") {
#if defined(_WIN32)
    SetEnvVarForTest("KOG_AI_HEARTBEAT_SECONDS", "1");
    SetEnvVarForTest("KOG_TEST_AI_HEARTBEAT", "1");

    CoutCapture capture;
    AiInvocationDiagnostics diagnostics;
    diagnostics.purpose = "plan-fill";
    diagnostics.resolvedProvider = "test-provider";
    diagnostics.responseFile = std::filesystem::path("C:/tmp/heartbeat-response.json");

    const auto result = ExecuteCommandWithHeartbeat(
        "cmd",
        {"/c", "timeout /t 2 /nobreak >nul & echo done"},
        kano::git::shell::ExecMode::Capture,
        std::nullopt,
        diagnostics);

    const auto output = capture.str();
    REQUIRE(result.exitCode == 0);
    REQUIRE(output.find("waiting for test-provider response") != std::string::npos);
    REQUIRE(output.find("response_exists=false") != std::string::npos);
    SetEnvVarForTest("KOG_AI_HEARTBEAT_SECONDS", "");
    SetEnvVarForTest("KOG_TEST_AI_HEARTBEAT", "");
#else
    SUCCEED("Windows-specific heartbeat test skipped on non-Windows platform");
#endif
}

TEST_CASE("ShouldIgnoreSecretFinding ignores dynamic Horde token assignments", "[Unit][CommitPlan][Diagnostics][SecretScan]") {
    using kano::git::commands::secret_scan::ShouldIgnoreSecretFinding;

    REQUIRE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "resolved_horde_token=\"$(build_resolve_horde_token || true)\""));
    REQUIRE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "DE_TOKEN=\"$UE_HORDE_TOKEN\""));
    REQUIRE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "DE_TOKEN=\"\\$UE_HORDE_TOKEN\""));
    REQUIRE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "DE_TOKEN=\"$(printenv \"KOG_TOKEN\")\""));
    REQUIRE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "api_key: \"env:CONTROL_PLANE_API_KEY\""));

    REQUIRE_FALSE(ShouldIgnoreSecretFinding(
        "generic_password_assignment",
        "api_token=\"abcd1234efgh5678\""));
}
