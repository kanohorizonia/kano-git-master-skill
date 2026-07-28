#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "commit_auto_plan_pipeline.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using kano::git::commands::NativeAiConfig;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& InName) {
        static std::atomic<unsigned long long> counter{0};
        path_ = (std::filesystem::temp_directory_path() /
                 ("kog-commit-auto-plan-" + InName + "-" + std::to_string(++counter)))
                    .lexically_normal();
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] auto Path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* InName, std::optional<std::string> InValue)
        : name_(InName) {
        if (const char* previous = std::getenv(InName); previous != nullptr) {
            previous_ = std::string(previous);
        }
        Set(std::move(InValue));
    }

    ~ScopedEnvironment() {
        Set(previous_);
    }

private:
    auto Set(const std::optional<std::string>& InValue) -> void {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), InValue.has_value() ? InValue->c_str() : "");
#else
        if (InValue.has_value()) {
            setenv(name_.c_str(), InValue->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

class StreamCapture {
public:
    StreamCapture()
        : previousOut_(std::cout.rdbuf(stdout_.rdbuf())),
          previousErr_(std::cerr.rdbuf(stderr_.rdbuf())) {}

    ~StreamCapture() {
        std::cout.rdbuf(previousOut_);
        std::cerr.rdbuf(previousErr_);
    }

    [[nodiscard]] auto Stdout() const -> std::string {
        return stdout_.str();
    }

    [[nodiscard]] auto Stderr() const -> std::string {
        return stderr_.str();
    }

private:
    std::ostringstream stdout_;
    std::ostringstream stderr_;
    std::streambuf* previousOut_;
    std::streambuf* previousErr_;
};

auto SplitTabs(const std::string& InLine) -> std::vector<std::string> {
    std::vector<std::string> fields;
    std::istringstream input(InLine);
    std::string field;
    while (std::getline(input, field, '\t')) {
        fields.push_back(field);
    }
    return fields;
}

auto ReadInvocations(const std::filesystem::path& InPath) -> std::vector<std::vector<std::string>> {
    std::ifstream input(InPath, std::ios::binary);
    std::vector<std::vector<std::string>> invocations;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        invocations.push_back(SplitTabs(line));
    }
    return invocations;
}

auto WriteText(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << InText;
}

auto ExpectedInvocation(const std::filesystem::path& InWorkspace,
                        std::initializer_list<std::string> InArgs) -> std::vector<std::string> {
    std::error_code ec;
    const auto canonicalWorkspace = std::filesystem::weakly_canonical(InWorkspace, ec);
    REQUIRE_FALSE(ec);
    std::vector<std::string> expected = {
        "cwd=" + canonicalWorkspace.generic_string(),
    };
    expected.insert(expected.end(), InArgs.begin(), InArgs.end());
    return expected;
}

class FakeSelfEnvironment {
public:
    explicit FakeSelfEnvironment(const std::filesystem::path& InLogPath)
        : binaryPath_("KANO_GIT_BINARY_PATH", std::string(KOG_TEST_FAKE_SELF_BINARY)),
          logPath_("KOG_TEST_FAKE_SELF_LOG", InLogPath.generic_string()),
          stdoutText_("KOG_TEST_FAKE_SELF_STDOUT", std::nullopt),
          stderrText_("KOG_TEST_FAKE_SELF_STDERR", std::nullopt),
          failContains_("KOG_TEST_FAKE_SELF_FAIL_CONTAINS", std::nullopt),
          exitCode_("KOG_TEST_FAKE_SELF_EXIT_CODE", std::nullopt),
          planPath_("KOG_PLAN_FILE", std::nullopt),
          debug_("KOG_DEBUG", std::nullopt) {}

private:
    ScopedEnvironment binaryPath_;
    ScopedEnvironment logPath_;
    ScopedEnvironment stdoutText_;
    ScopedEnvironment stderrText_;
    ScopedEnvironment failContains_;
    ScopedEnvironment exitCode_;
    ScopedEnvironment planPath_;
    ScopedEnvironment debug_;
};

} // namespace

TEST_CASE("auto-plan direct self commands preserve cwd arguments and captured output",
          "[commit-auto-plan][fake-self][characterization]") {
    TemporaryDirectory temp("direct-self");
    const auto workspace = temp.Path() / "workspace with space";
    const auto planPath = temp.Path() / "plan with space.json";
    const auto logPath = temp.Path() / "invocations.log";
    std::filesystem::create_directories(workspace);
    FakeSelfEnvironment environment(logPath);
    ScopedEnvironment stdoutText("KOG_TEST_FAKE_SELF_STDOUT", std::string("fake-stdout"));
    ScopedEnvironment stderrText("KOG_TEST_FAKE_SELF_STDERR", std::string("fake-stderr"));

    StreamCapture capture;
    REQUIRE(kano::git::commands::RunPlanNewViaSelf(workspace, planPath) == 0);
    REQUIRE(kano::git::commands::RunCommitSeedViaSelf(workspace, planPath) == 0);

    const auto invocations = ReadInvocations(logPath);
    REQUIRE(invocations.size() == 2);
    REQUIRE(invocations[0] == ExpectedInvocation(
        workspace,
        {"plan", "new", "--force", "--output", planPath.generic_string()}));
    REQUIRE(invocations[1] == ExpectedInvocation(
        workspace,
        {"plan", "commit-seed", "--force", "--deterministic",
         "--plan-file", planPath.generic_string()}));
    REQUIRE(capture.Stdout() == "fake-stdout\nfake-stdout\n");
    REQUIRE(capture.Stderr() == "fake-stderr\nfake-stderr\n");
}

TEST_CASE("nested self launcher shell leakage keeps the historical 127 refusal",
          "[commit-auto-plan][fake-self][characterization]") {
    TemporaryDirectory temp("nested-shell");
    const auto workspace = temp.Path() / "workspace";
    const auto planPath = temp.Path() / "plan.json";
    const auto logPath = temp.Path() / "invocations.log";
    std::filesystem::create_directories(workspace);
    FakeSelfEnvironment environment(logPath);
    ScopedEnvironment stderrText(
        "KOG_TEST_FAKE_SELF_STDERR",
        std::string("sh: find_binary: command not found"));

    StreamCapture capture;
    REQUIRE(kano::git::commands::RunPlanNewViaSelf(workspace, planPath) == 127);
    REQUIRE(capture.Stderr().find("find_binary` leaked into sh") != std::string::npos);
    REQUIRE(capture.Stderr().find("plan new failed via native binary (exit=127)") !=
            std::string::npos);
}

TEST_CASE("ignore compatibility failures remain nonfatal before deterministic refusal",
          "[commit-auto-plan][fake-self][characterization]") {
    const std::vector<std::pair<std::string, std::string>> scenarios = {
        {"plan runbook ignore", "state drift"},
        {"plan apply --stage ignore", "ignore plan entries"},
    };

    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        TemporaryDirectory temp("ignore-compat-" + std::to_string(index));
        const auto workspace = temp.Path() / "workspace";
        const auto planPath = temp.Path() / "missing-plan.json";
        const auto logPath = temp.Path() / "invocations.log";
        std::filesystem::create_directories(workspace);
        FakeSelfEnvironment environment(logPath);
        ScopedEnvironment planOverride("KOG_PLAN_FILE", planPath.generic_string());
        ScopedEnvironment failMatch(
            "KOG_TEST_FAKE_SELF_FAIL_CONTAINS",
            scenarios[index].first);
        ScopedEnvironment failCode("KOG_TEST_FAKE_SELF_EXIT_CODE", std::string("19"));
        ScopedEnvironment stderrText(
            "KOG_TEST_FAKE_SELF_STDERR",
            scenarios[index].second);

        NativeAiConfig ai;
        ai.enabled = true;
        ai.provider = "copilot";

        StreamCapture capture;
        REQUIRE(kano::git::commands::RunCommitAutoPlanPipeline(
                    workspace, ai, "single", false, false) == 2);
        REQUIRE(ReadInvocations(logPath).size() == 5);
        REQUIRE(capture.Stdout().find(
                    index == 0
                        ? "ignore runbook: no artifact candidates or plan already up-to-date; skipping"
                        : "ignore plan stage is empty; skipping ignore apply") !=
                std::string::npos);
        REQUIRE(capture.Stderr().find(
                    "AI commit runbook produced non-AI deterministic plan metadata") !=
                std::string::npos);
    }
}

TEST_CASE("fallback marker bypasses deterministic metadata refusal",
          "[commit-auto-plan][fake-self][characterization]") {
    TemporaryDirectory temp("fallback-marker");
    const auto workspace = temp.Path() / "workspace";
    const auto planPath = temp.Path() / "deterministic-plan.json";
    const auto logPath = temp.Path() / "invocations.log";
    std::filesystem::create_directories(workspace);
    WriteText(
        planPath,
        R"({"meta":{"planner":{"provider":"native","ai-model":"deterministic"}}})");
    FakeSelfEnvironment environment(logPath);
    ScopedEnvironment planOverride("KOG_PLAN_FILE", planPath.generic_string());
    ScopedEnvironment stdoutText(
        "KOG_TEST_FAKE_SELF_STDOUT",
        std::string("[plan] fallback_used: true\n"));

    NativeAiConfig ai;
    ai.enabled = true;
    ai.provider = "copilot";

    StreamCapture capture;
    const auto exitCode = kano::git::commands::RunCommitAutoPlanPipeline(
        workspace, ai, "single", false, false);
    INFO("post-runbook pre-commit exit=" << exitCode);
    REQUIRE(ReadInvocations(logPath).size() == 5);
    REQUIRE(capture.Stderr().find(
                "AI commit runbook produced non-AI deterministic plan metadata") ==
            std::string::npos);
}

TEST_CASE("commit and amend auto-plan pipelines preserve self-runbook order and arguments",
          "[commit-auto-plan][fake-self][characterization]") {
    TemporaryDirectory temp("pipeline-order");
    const auto workspace = temp.Path() / "workspace";
    const auto planPath = temp.Path() / "shared plan.json";
    std::filesystem::create_directories(workspace);

    NativeAiConfig ai;
    ai.enabled = true;
    ai.yolo = true;
    ai.provider = "copilot";
    ai.model = "provider model";

    for (const bool bAmend : {false, true}) {
        const auto logPath =
            temp.Path() / (bAmend ? "amend-invocations.log" : "commit-invocations.log");
        FakeSelfEnvironment environment(logPath);
        ScopedEnvironment planOverride("KOG_PLAN_FILE", planPath.generic_string());
        ScopedEnvironment failMatch(
            "KOG_TEST_FAKE_SELF_FAIL_CONTAINS",
            std::string("plan runbook commit"));
        ScopedEnvironment failCode("KOG_TEST_FAKE_SELF_EXIT_CODE", std::string("47"));
        ScopedEnvironment stdoutText(
            "KOG_TEST_FAKE_SELF_STDOUT",
            std::string("[plan] ai_fill_ms: 23\n"));
        ScopedEnvironment stderrText(
            "KOG_TEST_FAKE_SELF_STDERR",
            std::string("[plan] fallback_used: true\n"));

        StreamCapture capture;
        const auto exitCode = bAmend
            ? kano::git::commands::RunAmendAutoPlanPipeline(
                  workspace, ai, "per-commit", true, true)
            : kano::git::commands::RunCommitAutoPlanPipeline(
                  workspace, ai, "per-commit", true, true);
        REQUIRE(exitCode == 47);

        const auto invocations = ReadInvocations(logPath);
        REQUIRE(invocations.size() == 5);
        REQUIRE(invocations[0] == ExpectedInvocation(
            workspace,
            {"plan", "new", "--force", "--output", planPath.generic_string()}));
        REQUIRE(invocations[1] == ExpectedInvocation(
            workspace,
            {"plan", "runbook", "ignore", "--force",
             "--plan-file", planPath.generic_string()}));
        REQUIRE(invocations[2] == ExpectedInvocation(
            workspace,
            {"plan", "apply", "--stage", "ignore",
             "--plan-file", planPath.generic_string()}));
        REQUIRE(invocations[3] == ExpectedInvocation(
            workspace,
            {"plan", "commit-seed", "--force", "--deterministic",
             "--plan-file", planPath.generic_string()}));
        REQUIRE(invocations[4] == ExpectedInvocation(
            workspace,
            {"plan", "runbook", "commit",
             "--plan-file", planPath.generic_string(),
             "--ai-provider", "copilot",
             "--ai-model", "provider model",
             "--ai-fill-mode", "per-commit",
             "--allow-empty-dirty", "--yolo"}));

        REQUIRE(capture.Stderr().find(
                    "AI commit runbook failed via native binary (exit=47)") !=
                std::string::npos);
        REQUIRE(capture.Stdout().find("Auto-Plan Profile Summary") == std::string::npos);
    }
}
