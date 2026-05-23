#include <catch2/catch_test_macros.hpp>

#include "commit_ai_utils.hpp"
#include "ai_utils.hpp"
#include "kog_config.hpp"

#include <filesystem>
#include <vector>

using namespace kano::git::commands;

TEST_CASE("ResolveModelResolutionForAi uses provider-native auto for copilot", "[unit][ai][model-resolution]") {
    const auto workspace = std::filesystem::temp_directory_path();
    const auto resolved = ResolveModelResolutionForAi("copilot", "auto", false, workspace);

    REQUIRE(resolved.provider == "copilot");
    REQUIRE(resolved.modelMode == "provider-auto");
    REQUIRE(resolved.modelValue == "auto");
    REQUIRE_FALSE(resolved.fallbackUsed);
}

TEST_CASE("ResolveModelResolutionForAi falls back to kog-auto for providers without native auto", "[unit][ai][model-resolution]") {
    const auto workspace = std::filesystem::temp_directory_path();
    const auto resolved = ResolveModelResolutionForAi("codex", "auto", false, workspace);

    REQUIRE(resolved.provider == "codex");
    REQUIRE(resolved.modelMode == "kog-auto");
    REQUIRE(resolved.modelValue == "gpt-5.2-codex");
    REQUIRE(resolved.fallbackUsed);
    REQUIRE(resolved.fallbackReason == "provider-auto-unsupported");
}

TEST_CASE("ResolveModelResolutionForAi supports provider-default and explicit modes", "[unit][ai][model-resolution]") {
    const auto workspace = std::filesystem::temp_directory_path();

    const auto providerDefault = ResolveModelResolutionForAi("copilot", "provider-default", false, workspace);
    REQUIRE(providerDefault.modelMode == "provider-default");
    REQUIRE(providerDefault.modelValue.empty());

    const auto explicitModel = ResolveModelResolutionForAi("copilot", "gpt-5-mini", false, workspace);
    REQUIRE(explicitModel.modelMode == "explicit");
    REQUIRE(explicitModel.modelValue == "gpt-5-mini");
}

TEST_CASE("NormalizeAiModelSelection recognizes provider-auto and kog-auto", "[unit][ai][model-selection]") {
    REQUIRE(kog_config::NormalizeAiModelSelection("provider-auto") == "provider-auto");
    REQUIRE(kog_config::NormalizeAiModelSelection("kog-auto") == "kog-auto");
    REQUIRE(kog_config::NormalizeAiModelSelection("default") == "provider-default");
}

TEST_CASE("AppendModelArgsForProvider emits mode-aware copilot model args", "[unit][ai][model-args]") {
    std::vector<std::string> args;
    AppendModelArgsForProvider(args, "copilot", "provider-auto", "auto");
    REQUIRE(args == std::vector<std::string>{"--model", "auto"});

    args.clear();
    AppendModelArgsForProvider(args, "copilot", "provider-default", "");
    REQUIRE(args.empty());

    args.clear();
    AppendModelArgsForProvider(args, "copilot", "kog-auto", "copilot/gpt-5.4?reasoning=high");
    REQUIRE(args == std::vector<std::string>{"--model", "gpt-5.4", "--reasoning-effort", "high"});
}
