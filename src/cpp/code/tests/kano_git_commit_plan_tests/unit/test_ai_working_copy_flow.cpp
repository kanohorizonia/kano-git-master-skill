#include <catch2/catch_test_macros.hpp>

#include "plan_utils.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace kano::git::commands;

namespace {

struct ScopedEnv {
    std::string name;
    bool hadPrevious = false;
    std::string previous;

    ScopedEnv(const char* inName, const char* inValue) : name(inName) {
        if (const char* prev = std::getenv(inName); prev != nullptr) {
            hadPrevious = true;
            previous = prev;
        }
#if defined(_WIN32)
        _putenv_s(inName, inValue != nullptr ? inValue : "");
#else
        if (inValue != nullptr) {
            setenv(inName, inValue, 1);
        } else {
            unsetenv(inName);
        }
#endif
    }

    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name.c_str(), hadPrevious ? previous.c_str() : "");
#else
        if (hadPrevious) {
            setenv(name.c_str(), previous.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

auto PathEnvSeparator() -> const char* {
#if defined(_WIN32)
    return ";";
#else
    return ":";
#endif
}

auto UniqueTempWorkspace(const std::string& suffix) -> std::filesystem::path {
    const auto root = (std::filesystem::temp_directory_path() / std::filesystem::path("kog-plan-ai-tests") / std::filesystem::path(suffix)).lexically_normal();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

auto BuildSingleEntryPlan(const std::filesystem::path& workspaceRoot,
                          const std::string& message,
                          const std::string& reviewReason) -> std::string {
    auto plan = BuildDefaultPlanTemplate(workspaceRoot);
    const auto updated = UpsertCommitEntry(plan,
                                           ".",
                                           message,
                                           {},
                                           {},
                                           "pass",
                                           reviewReason);
    REQUIRE(updated.has_value());
    return *updated;
}

void InstallFakeCopilot(const std::filesystem::path& workspaceRoot) {
    const auto binDir = (workspaceRoot / "fake-bin").lexically_normal();
    const auto cmdPath = (binDir / "copilot.cmd").lexically_normal();
    const auto psPath = (binDir / "fake_copilot.ps1").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(binDir, ec);
    REQUIRE(!ec);

    const auto cmd =
        "@echo off\r\n"
        "powershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0fake_copilot.ps1\" %*\r\n"
        "exit /b %ERRORLEVEL%\r\n";
    std::string error;
    REQUIRE(WriteFileText(cmdPath, cmd, &error));

    const auto script = R"PS(
$planPath = $null
$gitignorePath = $null
for ($i = 0; $i -lt $args.Length; $i++) {
  if ($args[$i] -eq '--allow-tool' -and ($i + 1) -lt $args.Length) {
    $tool = $args[$i + 1]
    if ($tool -match '^write\((.*)\)$') {
      if (-not $planPath) { $planPath = $Matches[1] }
      elseif (-not $gitignorePath) { $gitignorePath = $Matches[1] }
    }
    $i++
  }
}

if ($env:KOG_TEST_PLAN_SOURCE -and $planPath) {
  $planContent = Get-Content -Raw -LiteralPath $env:KOG_TEST_PLAN_SOURCE
  Set-Content -LiteralPath $planPath -Value $planContent -NoNewline
}

if ($env:KOG_TEST_GITIGNORE_APPEND -and $gitignorePath) {
  $existing = ''
  if (Test-Path -LiteralPath $gitignorePath) {
    $existing = Get-Content -Raw -LiteralPath $gitignorePath
  }
  if ($existing.Length -gt 0 -and -not $existing.EndsWith("`n")) {
    $existing += "`n"
  }
  $lines = @()
  if ($existing.Length -gt 0) {
    $lines = $existing -split "`r?`n"
  }
  if (-not ($lines | Where-Object { $_ -eq $env:KOG_TEST_GITIGNORE_APPEND })) {
    $existing += "$($env:KOG_TEST_GITIGNORE_APPEND)`n"
  }
  Set-Content -LiteralPath $gitignorePath -Value $existing -NoNewline
}

Write-Output "Done."
)PS";
    REQUIRE(WriteFileText(psPath, script, &error));
}

auto LoadJson(const std::filesystem::path& path) -> nlohmann::json {
    const auto text = ReadFileText(path);
    REQUIRE(text.has_value());
    return nlohmann::json::parse(*text);
}

} // namespace

TEST_CASE("FillPlanByAi promotes edited working plan and merges new gitignore rules in single mode",
          "[tdd][unit][feature:ai-provider-bootstrap][FillPlanByAi][working-copy][gitignore]") {
    const auto workspaceRoot = UniqueTempWorkspace("single-success");
    InstallFakeCopilot(workspaceRoot);

    const auto planPath = (workspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
    const auto gitignorePath = (workspaceRoot / ".gitignore").lexically_normal();
    const auto sourcePlanPath = (workspaceRoot / "expected-plan.json").lexically_normal();

    std::string error;
    REQUIRE(WriteFileText(gitignorePath, "existing-rule/\n", &error));

    const auto initialPlan = BuildSingleEntryPlan(workspaceRoot,
                                                  "replace-with-commit-message",
                                                  "replace-with-review-reason");
    REQUIRE(WriteFileText(planPath, initialPlan, &error));

    const auto replacementCommitStage = R"===([
  {
    "repo": ".",
    "commits": [
      {
        "message": "[Build][Feature] Promote validated working-copy plan edits (NO-TICKET)",
        "include": ["src/cpp/code/systems/kano_git_command/commit_plan/private/plan_utils.cpp"],
        "exclude": [],
        "review": {
          "verdict": "pass",
          "reason": "Only the working-copy plan promotion changes are included and no secrets or generated artifacts are part of this commit."
        }
      }
    ]
  }
])===";
    const auto candidatePlan = ApplyCommitStageReplacement(initialPlan, replacementCommitStage, &error);
    REQUIRE(candidatePlan.has_value());
    REQUIRE(WriteFileText(sourcePlanPath, *candidatePlan, &error));

    const auto originalPath = std::getenv("PATH") != nullptr ? std::string(std::getenv("PATH")) : std::string();
    const auto fakePath = (workspaceRoot / "fake-bin").lexically_normal().string() + PathEnvSeparator() + originalPath;
    const ScopedEnv pathEnv("PATH", fakePath.c_str());
    const ScopedEnv planEnv("KOG_TEST_PLAN_SOURCE", sourcePlanPath.string().c_str());
    const ScopedEnv gitignoreEnv("KOG_TEST_GITIGNORE_APPEND", "new-generated-rule/");

    std::string fillError;
    REQUIRE(FillPlanByAi(workspaceRoot, planPath, "copilot", "gpt-5.4", "single", false, &fillError, true));
    REQUIRE(fillError.empty());

    const auto finalDoc = LoadJson(planPath);
    REQUIRE(finalDoc["stages"]["commit"][0]["commits"][0]["message"].get<std::string>() ==
            "[Build][Feature] Promote validated working-copy plan edits (NO-TICKET)");
    REQUIRE(finalDoc["meta"]["planner"]["provider"].get<std::string>() == "copilot");
    REQUIRE(finalDoc["meta"]["planner"]["ai-model"].get<std::string>() == "gpt-5.4");

    const auto gitignoreText = ReadFileText(gitignorePath);
    REQUIRE(gitignoreText.has_value());
    REQUIRE(gitignoreText->find("existing-rule/") != std::string::npos);
    REQUIRE(gitignoreText->find("new-generated-rule/") != std::string::npos);
}

TEST_CASE("FillPlanByAi leaves authoritative plan unchanged when edited working plan fails validation",
          "[tdd][unit][feature:ai-provider-bootstrap][FillPlanByAi][working-copy][validation]") {
    const auto workspaceRoot = UniqueTempWorkspace("single-invalid");
    InstallFakeCopilot(workspaceRoot);

    const auto planPath = (workspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
    const auto sourcePlanPath = (workspaceRoot / "invalid-plan.json").lexically_normal();

    std::string error;
    const auto initialPlan = BuildSingleEntryPlan(workspaceRoot,
                                                  "replace-with-commit-message",
                                                  "replace-with-review-reason");
    REQUIRE(WriteFileText(planPath, initialPlan, &error));
    REQUIRE(WriteFileText(sourcePlanPath, initialPlan, &error));

    const auto originalPath = std::getenv("PATH") != nullptr ? std::string(std::getenv("PATH")) : std::string();
    const auto fakePath = (workspaceRoot / "fake-bin").lexically_normal().string() + PathEnvSeparator() + originalPath;
    const ScopedEnv pathEnv("PATH", fakePath.c_str());
    const ScopedEnv planEnv("KOG_TEST_PLAN_SOURCE", sourcePlanPath.string().c_str());
    const ScopedEnv gitignoreEnv("KOG_TEST_GITIGNORE_APPEND", "");

    std::string fillError;
    REQUIRE_FALSE(FillPlanByAi(workspaceRoot, planPath, "copilot", "gpt-5.4", "single", false, &fillError, true));
    REQUIRE_FALSE(fillError.empty());

    const auto finalText = ReadFileText(planPath);
    REQUIRE(finalText.has_value());
    REQUIRE(*finalText == initialPlan);
}

TEST_CASE("FillPlanByAi uses working-copy flow for per-commit mode",
          "[tdd][unit][feature:ai-provider-bootstrap][FillPlanByAi][working-copy][per-commit]") {
    const auto workspaceRoot = UniqueTempWorkspace("per-commit-success");
    InstallFakeCopilot(workspaceRoot);

    const auto planPath = (workspaceRoot / ".kano" / "tmp" / "git" / "plans" / "default-plan.json").lexically_normal();
    const auto sourcePlanPath = (workspaceRoot / "expected-plan.json").lexically_normal();

    std::string error;
    const auto initialPlan = BuildSingleEntryPlan(workspaceRoot,
                                                  "[Seed][Chore] Deterministic placeholder scope (NO-TICKET)",
                                                  "Deterministic placeholder review reason.");
    REQUIRE(WriteFileText(planPath, initialPlan, &error));

    const auto replacementCommitStage = R"===([
  {
    "repo": ".",
    "commits": [
      {
        "message": "[CommitPlan][Feature] Use working-copy edits for per-commit fill (NO-TICKET)",
        "include": [],
        "exclude": [],
        "review": {
          "verdict": "pass",
          "reason": "This entry only updates the per-commit working-copy flow and excludes secrets, generated files, and unrelated changes."
        }
      }
    ]
  }
])===";
    const auto candidatePlan = ApplyCommitStageReplacement(initialPlan, replacementCommitStage, &error);
    REQUIRE(candidatePlan.has_value());
    REQUIRE(WriteFileText(sourcePlanPath, *candidatePlan, &error));

    const auto originalPath = std::getenv("PATH") != nullptr ? std::string(std::getenv("PATH")) : std::string();
    const auto fakePath = (workspaceRoot / "fake-bin").lexically_normal().string() + PathEnvSeparator() + originalPath;
    const ScopedEnv pathEnv("PATH", fakePath.c_str());
    const ScopedEnv planEnv("KOG_TEST_PLAN_SOURCE", sourcePlanPath.string().c_str());
    const ScopedEnv gitignoreEnv("KOG_TEST_GITIGNORE_APPEND", "");

    std::string fillError;
    REQUIRE(FillPlanByAi(workspaceRoot, planPath, "copilot", "gpt-5.4", "per-commit", false, &fillError, true));
    REQUIRE(fillError.empty());

    const auto finalDoc = LoadJson(planPath);
    REQUIRE(finalDoc["stages"]["commit"][0]["commits"][0]["message"].get<std::string>() ==
            "[CommitPlan][Feature] Use working-copy edits for per-commit fill (NO-TICKET)");
    REQUIRE(finalDoc["meta"]["planner"]["provider"].get<std::string>() == "copilot");
    REQUIRE(finalDoc["meta"]["planner"]["ai-model"].get<std::string>() == "gpt-5.4");
}

TEST_CASE("FillPlanByAi restores missing commit review verdicts in edited working plans",
          "[tdd][unit][feature:ai-provider-bootstrap][FillPlanByAi][working-copy][review-verdict][deterministic]") {
    const auto workspaceRoot = UniqueTempWorkspace("single-missing-review-verdict");
    const auto initialPlan = BuildSingleEntryPlan(workspaceRoot,
                                                  "replace-with-commit-message",
                                                  "replace-with-review-reason");
    auto candidateDoc = nlohmann::json::parse(initialPlan);
    candidateDoc["stages"]["commit"][0]["commits"][0]["message"] =
        "[Build][Chore] Normalize AI plan review verdict defaults (NO-TICKET)";
    candidateDoc["stages"]["commit"][0]["commits"][0]["include"] = nlohmann::json::array(
        {"src/cpp/code/systems/kano_git_command/commit_plan/private/plan_utils.cpp"});
    candidateDoc["stages"]["commit"][0]["commits"][0]["review"]["reason"] =
        "Only commit-plan working-copy normalization logic is included and no unrelated files or secrets are part of this commit.";
    candidateDoc["stages"]["commit"][0]["commits"][0]["review"].erase("verdict");

    std::string validationError;
    REQUIRE_FALSE(ValidateAiReadyPlan(candidateDoc.dump(), &validationError));

    const auto normalizedPlan = NormalizeAiReadyPlanReviewVerdicts(candidateDoc.dump());
    REQUIRE(normalizedPlan.has_value());
    REQUIRE(ValidateAiReadyPlan(*normalizedPlan, &validationError));

    const auto finalDoc = nlohmann::json::parse(*normalizedPlan);
    REQUIRE(finalDoc["stages"]["commit"][0]["commits"][0]["review"]["verdict"].get<std::string>() == "pass");
    REQUIRE(finalDoc["stages"]["commit"][0]["commits"][0]["review"]["reason"].get<std::string>() ==
            "Only commit-plan working-copy normalization logic is included and no unrelated files or secrets are part of this commit.");
}
