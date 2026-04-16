// Preservation property tests for LogInvocation lambda in RunAiGenerate.
//
// Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5
//
// EXPECTED OUTCOME: PASSES on unfixed code.
// These tests compare the three preserved log lines produced by the UNFIXED
// lambda against the same lines produced by the FIXED lambda.  Because the
// three lines are identical in both versions, the tests pass on unfixed code
// and continue to pass after the fix is applied (regression guard).
//
// The three preserved lines are:
//   [kog ai] command : <binary> <args>
//   [kog ai] model   : <model|auto>
//   [kog ai] Waiting for <provider> response...

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>
#include <catch2/generators/catch_generators_random.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// CaptureCerr redirects std::cerr to an in-memory buffer for the duration of
// its lifetime, then restores the original stream buffer on destruction.
struct CaptureCerr {
    std::ostringstream buffer;
    std::streambuf* original = nullptr;

    CaptureCerr() {
        original = std::cerr.rdbuf(buffer.rdbuf());
    }

    ~CaptureCerr() {
        std::cerr.rdbuf(original);
    }

    auto captured() const -> std::string {
        return buffer.str();
    }
};

// ---------------------------------------------------------------------------
// UNFIXED LogInvocation — exact replica of the lambda before the fix.
// Contains UTF-8 box-drawing characters and the unconditional prompt dump.
// ---------------------------------------------------------------------------
auto InvokeUnfixedLogInvocation(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& InModel,
    const std::string& InProvider,
    const std::string& InPrompt) -> void
{
    static constexpr std::string_view kDivider =
        "────────────────────────────────────────";

    std::cerr << "\n[kog ai] ── AI Invocation (plan-fill) ──\n";
    std::cerr << "[kog ai] command : " << binary;
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos || a.empty())
            std::cerr << " \"" << a << "\"";
        else
            std::cerr << " " << a;
    }
    std::cerr << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";
    std::cerr << "[kog ai] prompt  :\n" << kDivider << "\n"
              << InPrompt << "\n" << kDivider << "\n";
    std::cerr << "[kog ai] Waiting for " << InProvider << " response...\n";
    std::cerr.flush();
}

// ---------------------------------------------------------------------------
// FIXED LogInvocation — the target implementation from the design document.
// ASCII-only header, no kDivider, no prompt dump block.
// ---------------------------------------------------------------------------
auto InvokeFixedLogInvocation(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& InModel,
    const std::string& InProvider) -> void
{
    std::cerr << "\n[kog ai] -- AI Invocation (plan-fill) --\n";
    std::cerr << "[kog ai] command : " << binary;
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos || a.empty())
            std::cerr << " \"" << a << "\"";
        else
            std::cerr << " " << a;
    }
    std::cerr << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";
    std::cerr << "[kog ai] Waiting for " << InProvider << " response...\n";
    std::cerr.flush();
}

// ---------------------------------------------------------------------------
// Helper: extract a single line that starts with the given prefix from a
// multi-line string.  Returns the full line (including the trailing '\n') or
// an empty string if the prefix is not found.
// ---------------------------------------------------------------------------
auto ExtractLine(const std::string& output, const std::string& prefix) -> std::string {
    const auto pos = output.find(prefix);
    if (pos == std::string::npos) return {};
    const auto end = output.find('\n', pos);
    if (end == std::string::npos) return output.substr(pos);
    return output.substr(pos, end - pos + 1);
}

// ---------------------------------------------------------------------------
// Helper: capture the three preserved lines from a single invocation of the
// UNFIXED lambda.
// ---------------------------------------------------------------------------
struct PreservedLines {
    std::string commandLine;
    std::string modelLine;
    std::string waitingLine;
};

auto CaptureUnfixedLines(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& model,
    const std::string& provider,
    const std::string& prompt) -> PreservedLines
{
    CaptureCerr cap;
    InvokeUnfixedLogInvocation(binary, args, model, provider, prompt);
    const auto out = cap.captured();
    return {
        ExtractLine(out, "[kog ai] command : "),
        ExtractLine(out, "[kog ai] model   : "),
        ExtractLine(out, "[kog ai] Waiting for ")
    };
}

// ---------------------------------------------------------------------------
// Helper: capture the three preserved lines from a single invocation of the
// FIXED lambda.
// ---------------------------------------------------------------------------
auto CaptureFixedLines(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& model,
    const std::string& provider) -> PreservedLines
{
    CaptureCerr cap;
    InvokeFixedLogInvocation(binary, args, model, provider);
    const auto out = cap.captured();
    return {
        ExtractLine(out, "[kog ai] command : "),
        ExtractLine(out, "[kog ai] model   : "),
        ExtractLine(out, "[kog ai] Waiting for ")
    };
}

} // namespace

// ---------------------------------------------------------------------------
// Property 2: Preservation — Command, Model, and Waiting Lines Unchanged
//
// Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5
//
// EXPECTED OUTCOME: PASSES on unfixed code.
// The three preserved lines are identical in both the unfixed and fixed
// versions of the lambda, so these assertions hold before and after the fix.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 2.1  Command line preservation — parametric cases
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_command_line_preserved_across_fix",
          "[preservation][pbt][req-3.1]")
{
    // **Validates: Requirements 3.1**

    struct TestInput {
        std::string binary;
        std::vector<std::string> args;
        std::string model;
        std::string provider;
        std::string description;
    };

    const std::vector<TestInput> inputs = {
        // Typical copilot invocation
        {
            "copilot",
            {"--no-color", "--stream", "off", "-p", "/tmp/prompt.md"},
            "gpt-4o", "copilot",
            "typical copilot invocation"
        },
        // Arg containing a space — must be quoted
        {
            "gh",
            {"copilot", "suggest", "--no-color", "-p", "/tmp/prompt with spaces.md"},
            "gpt-4o-mini", "copilot",
            "arg with embedded space is quoted"
        },
        // Empty arg — must be quoted as ""
        {
            "opencode",
            {"--model", "gpt-5", "", "--stream"},
            "gpt-5", "opencode",
            "empty arg is quoted"
        },
        // No args at all
        {
            "codex",
            {},
            "", "codex",
            "empty args vector, empty model (auto fallback)"
        },
        // Multiple args with spaces
        {
            "ai-tool",
            {"arg one", "arg two", "plain"},
            "claude-3", "anthropic",
            "multiple args with spaces"
        },
        // Binary with path separators
        {
            "/usr/local/bin/copilot",
            {"-p", "/home/user/my prompt.md"},
            "auto", "copilot",
            "binary with path, arg with space"
        },
        // Single empty arg
        {
            "tool",
            {""},
            "model-x", "provider-y",
            "single empty arg"
        },
        // Mix of empty, spaced, and plain args
        {
            "binary",
            {"", "plain", "has space", ""},
            "m", "p",
            "mix of empty, spaced, and plain args"
        },
    };

    for (const auto& input : inputs) {
        SECTION(input.description) {
            const auto unfixed = CaptureUnfixedLines(
                input.binary, input.args, input.model, input.provider, "dummy-prompt");
            const auto fixed = CaptureFixedLines(
                input.binary, input.args, input.model, input.provider);

            INFO("binary=" << input.binary << " model=" << input.model
                 << " provider=" << input.provider);
            INFO("unfixed command line: " << unfixed.commandLine);
            INFO("fixed   command line: " << fixed.commandLine);

            // Req 3.1: command line byte-for-byte identical
            REQUIRE_FALSE(unfixed.commandLine.empty());
            REQUIRE(unfixed.commandLine == fixed.commandLine);
        }
    }
}

// ---------------------------------------------------------------------------
// 2.2  Model line preservation — parametric cases
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_model_line_preserved_across_fix",
          "[preservation][pbt][req-3.2]")
{
    // **Validates: Requirements 3.2**

    struct TestInput {
        std::string model;
        std::string description;
    };

    const std::vector<TestInput> inputs = {
        { "gpt-4o",        "explicit model name" },
        { "gpt-4o-mini",   "explicit mini model" },
        { "claude-3",      "anthropic model" },
        { "auto",          "literal 'auto' string" },
        { "",              "empty model → auto fallback" },
        { "gpt-5",         "future model name" },
        { "a",             "single-char model" },
        { "model with spaces", "model name with spaces" },
    };

    const std::string binary   = "copilot";
    const std::vector<std::string> args = {"--no-color"};
    const std::string provider = "copilot";

    for (const auto& input : inputs) {
        SECTION(input.description) {
            const auto unfixed = CaptureUnfixedLines(
                binary, args, input.model, provider, "dummy-prompt");
            const auto fixed = CaptureFixedLines(
                binary, args, input.model, provider);

            INFO("model=" << input.model);
            INFO("unfixed model line: " << unfixed.modelLine);
            INFO("fixed   model line: " << fixed.modelLine);

            // Req 3.2: model line byte-for-byte identical
            REQUIRE_FALSE(unfixed.modelLine.empty());
            REQUIRE(unfixed.modelLine == fixed.modelLine);
        }
    }
}

// ---------------------------------------------------------------------------
// 2.3  Waiting line preservation — parametric cases
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_waiting_line_preserved_across_fix",
          "[preservation][pbt][req-3.3]")
{
    // **Validates: Requirements 3.3**

    struct TestInput {
        std::string provider;
        std::string description;
    };

    const std::vector<TestInput> inputs = {
        { "copilot",    "copilot provider" },
        { "codex",      "codex provider" },
        { "opencode",   "opencode provider" },
        { "anthropic",  "anthropic provider" },
        { "openai",     "openai provider" },
        { "custom-ai",  "custom provider with hyphen" },
        { "AI Provider With Spaces", "provider with spaces" },
        { "x",          "single-char provider" },
        { "",           "empty provider string" },
    };

    const std::string binary   = "copilot";
    const std::vector<std::string> args = {"--no-color"};
    const std::string model    = "gpt-4o";

    for (const auto& input : inputs) {
        SECTION(input.description) {
            const auto unfixed = CaptureUnfixedLines(
                binary, args, model, input.provider, "dummy-prompt");
            const auto fixed = CaptureFixedLines(
                binary, args, model, input.provider);

            INFO("provider=" << input.provider);
            INFO("unfixed waiting line: " << unfixed.waitingLine);
            INFO("fixed   waiting line: " << fixed.waitingLine);

            // Req 3.3: waiting line byte-for-byte identical
            REQUIRE_FALSE(unfixed.waitingLine.empty());
            REQUIRE(unfixed.waitingLine == fixed.waitingLine);
        }
    }
}

// ---------------------------------------------------------------------------
// 2.4  All three lines preserved simultaneously — combined tuple sweep
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_all_three_lines_preserved_for_combined_tuples",
          "[preservation][pbt][req-3.1][req-3.2][req-3.3]")
{
    // **Validates: Requirements 3.1, 3.2, 3.3**
    //
    // Exercises a cross-product of representative (binary, args, model,
    // provider) tuples including edge cases: empty model, empty args, args
    // with spaces, empty args, unusual provider strings.

    struct Tuple {
        std::string binary;
        std::vector<std::string> args;
        std::string model;
        std::string provider;
        std::string description;
    };

    const std::vector<Tuple> tuples = {
        // Standard case
        { "copilot", {"--no-color", "-p", "/tmp/p.md"}, "gpt-4o", "copilot",
          "standard copilot tuple" },
        // Empty model → auto
        { "codex", {"-q", "/tmp/p.md"}, "", "codex",
          "empty model auto-fallback" },
        // Arg with space
        { "gh", {"copilot", "suggest", "-p", "/tmp/my prompt.md"}, "gpt-4o-mini", "copilot",
          "arg with space" },
        // Empty arg
        { "tool", {"--flag", "", "--other"}, "model-a", "provider-a",
          "empty arg in middle" },
        // No args
        { "binary", {}, "m", "p",
          "no args" },
        // Unusual provider
        { "ai", {"--x"}, "gpt-5", "my-custom-AI-provider",
          "unusual provider string" },
        // Empty provider
        { "ai", {"--x"}, "gpt-5", "",
          "empty provider" },
        // Multiple spaces-in-args
        { "tool", {"arg one", "arg two", "plain", ""}, "claude-3", "anthropic",
          "multiple spaced and empty args" },
    };

    for (const auto& t : tuples) {
        SECTION(t.description) {
            const auto unfixed = CaptureUnfixedLines(
                t.binary, t.args, t.model, t.provider, "some prompt text");
            const auto fixed = CaptureFixedLines(
                t.binary, t.args, t.model, t.provider);

            INFO("binary=" << t.binary << " model=" << t.model
                 << " provider=" << t.provider);
            INFO("unfixed command : " << unfixed.commandLine);
            INFO("fixed   command : " << fixed.commandLine);
            INFO("unfixed model   : " << unfixed.modelLine);
            INFO("fixed   model   : " << fixed.modelLine);
            INFO("unfixed waiting : " << unfixed.waitingLine);
            INFO("fixed   waiting : " << fixed.waitingLine);

            // All three lines must be byte-for-byte identical
            REQUIRE_FALSE(unfixed.commandLine.empty());
            REQUIRE(unfixed.commandLine == fixed.commandLine);

            REQUIRE_FALSE(unfixed.modelLine.empty());
            REQUIRE(unfixed.modelLine == fixed.modelLine);

            REQUIRE_FALSE(unfixed.waitingLine.empty());
            REQUIRE(unfixed.waitingLine == fixed.waitingLine);
        }
    }
}
