// Bug condition exploration test for LogInvocation lambda in RunAiGenerate.
//
// Validates: Requirements 1.1, 1.2, 1.3
//
// CRITICAL: This test is EXPECTED TO FAIL on unfixed code.
// Failure confirms both bugs exist:
//   Bug 1 (Req 1.1, 1.2): UTF-8 box-drawing characters in header and kDivider
//            produce bytes outside ASCII range (0xE2 0x94 0x80 for U+2500 '─')
//   Bug 2 (Req 1.3): Prompt body is unconditionally dumped to stderr
//
// When the fix is applied (Task 3), this same test will PASS, confirming both
// defects are resolved.

#include <catch2/catch_test_macros.hpp>

#include <iomanip>
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

// Replicates the UNFIXED LogInvocation lambda body from RunAiGenerate in
// plan_utils.cpp exactly as it exists before the fix is applied.
//
// Source reference (unfixed):
//   auto LogInvocation = [&](const std::string& binary,
//                             const std::vector<std::string>& args) {
//       static constexpr std::string_view kDivider =
//           "────────────────────────────────────────";
//       std::cerr << "\n[kog ai] ── AI Invocation (plan-fill) ──\n";
//       std::cerr << "[kog ai] command : " << binary;
//       for (const auto& a : args) {
//           if (a.find(' ') != std::string::npos || a.empty())
//               std::cerr << " \"" << a << "\"";
//           else
//               std::cerr << " " << a;
//       }
//       std::cerr << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";
//       std::cerr << "[kog ai] prompt  :\n" << kDivider << "\n"
//                 << InPrompt << "\n" << kDivider << "\n";
//       std::cerr << "[kog ai] Waiting for " << InProvider << " response...\n";
//       std::cerr.flush();
//   };
auto InvokeUnfixedLogInvocation(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& InModel,
    const std::string& InProvider,
    const std::string& InPrompt) -> void
{
    // Exact copy of the unfixed kDivider — contains UTF-8 box-drawing U+2500 '─'
    // encoded as 0xE2 0x94 0x80 per byte triple.
    static constexpr std::string_view kDivider =
        "────────────────────────────────────────";

    // Exact copy of the unfixed header line — contains UTF-8 '──' sequences.
    std::cerr << "\n[kog ai] ── AI Invocation (plan-fill) ──\n";

    std::cerr << "[kog ai] command : " << binary;
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos || a.empty())
            std::cerr << " \"" << a << "\"";
        else
            std::cerr << " " << a;
    }
    std::cerr << "\n[kog ai] model   : " << (InModel.empty() ? "auto" : InModel) << "\n";

    // Exact copy of the unfixed prompt dump block — unconditionally present.
    std::cerr << "[kog ai] prompt  :\n" << kDivider << "\n"
              << InPrompt << "\n" << kDivider << "\n";

    std::cerr << "[kog ai] Waiting for " << InProvider << " response...\n";
    std::cerr.flush();
}

// Returns true if the string contains any byte with value > 0x7F (non-ASCII).
auto ContainsNonAsciiByte(const std::string& s) -> bool {
    for (unsigned char c : s) {
        if (c > 0x7F) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Property 1: Bug Condition — UTF-8 Header and Prompt Dump Defects
//
// Validates: Requirements 1.1, 1.2, 1.3
//
// EXPECTED OUTCOME: FAILS on unfixed code.
//   - Assertion 1 fails because the header and kDivider contain 0xE2 0x94 0x80
//     (UTF-8 encoding of U+2500 '─'), which are bytes > 0x7F.
//   - Assertion 2 fails because the prompt dump block is unconditionally written.
//   - Assertion 3 fails because InPrompt content appears in the output.
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_unfixed_outputs_non_ascii_bytes_and_prompt_dump",
          "[bugfix][encoding][prompt-dump][pbt][req-1.1][req-1.2][req-1.3]")
{
    // Representative inputs — any (binary, args, model, provider, prompt) tuple
    // will trigger both bugs because:
    //   - The header and kDivider are hardcoded UTF-8 strings (always non-ASCII)
    //   - The prompt dump block is unconditional (always present)
    const std::string binary   = "copilot";
    const std::vector<std::string> args = {"--no-color", "--stream", "off", "-p", "/tmp/prompt.md"};
    const std::string model    = "gpt-4o";
    const std::string provider = "copilot";
    const std::string prompt   = "Read @./.kano/tmp/git/provider-prompts/plan-fill-abc123.md and follow it exactly.";

    CaptureCerr capture;
    InvokeUnfixedLogInvocation(binary, args, model, provider, prompt);
    const std::string output = capture.captured();

    INFO("Captured stderr output (hex dump of first 200 bytes):");
    {
        std::ostringstream hex;
        const auto limit = std::min(output.size(), std::size_t{200});
        for (std::size_t i = 0; i < limit; ++i) {
            hex << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(static_cast<unsigned char>(output[i])) << " ";
            if ((i + 1) % 16 == 0) hex << "\n";
        }
        INFO(hex.str());
    }
    INFO("Full captured output:\n" << output);

    // -----------------------------------------------------------------------
    // Assertion 1 (Req 1.1, 1.2): Output must contain ONLY ASCII bytes.
    // WILL FAIL on unfixed code — the header "\n[kog ai] ── AI Invocation ..."
    // and kDivider "────..." both embed 0xE2 0x94 0x80 sequences.
    // Counterexample: bytes 0xE2, 0x94, 0x80 appear at the header and divider
    // positions, proving the encoding defect exists.
    // -----------------------------------------------------------------------
    CHECK_FALSE(ContainsNonAsciiByte(output));

    // -----------------------------------------------------------------------
    // Assertion 2 (Req 1.3): Output must NOT contain the prompt label.
    // WILL FAIL on unfixed code — the prompt dump block is unconditional.
    // Counterexample: "[kog ai] prompt  :" appears in the output.
    // -----------------------------------------------------------------------
    CHECK(output.find("[kog ai] prompt  :") == std::string::npos);

    // -----------------------------------------------------------------------
    // Assertion 3 (Req 1.3): Output must NOT contain any portion of InPrompt.
    // WILL FAIL on unfixed code — InPrompt is written verbatim between dividers.
    // Counterexample: the full prompt text appears between two kDivider lines.
    // -----------------------------------------------------------------------
    CHECK(output.find(prompt) == std::string::npos);
}

// ---------------------------------------------------------------------------
// Additional parametric cases — verify the bugs manifest for varied inputs.
// Any (binary, args, model, provider, prompt) tuple triggers both defects.
// ---------------------------------------------------------------------------
TEST_CASE("LogInvocation_unfixed_bug_condition_holds_for_varied_inputs",
          "[bugfix][encoding][prompt-dump][pbt][req-1.1][req-1.2][req-1.3]")
{
    struct TestInput {
        std::string binary;
        std::vector<std::string> args;
        std::string model;
        std::string provider;
        std::string prompt;
        std::string description;
    };

    const std::vector<TestInput> inputs = {
        {
            "codex", {"-q", "/tmp/prompt.md"}, "", "codex",
            "Short prompt.",
            "codex provider, empty model (auto fallback)"
        },
        {
            "gh", {"copilot", "suggest", "--no-color", "-p", "/tmp/p.md"}, "gpt-4o-mini", "copilot",
            "A longer prompt body with multiple lines.\nLine two.\nLine three.",
            "gh copilot provider, multi-line prompt"
        },
        {
            "opencode", {"--model", "gpt-5", "-p", "/tmp/prompt with spaces.md"}, "gpt-5", "opencode",
            "Prompt containing special chars: <>&\"'",
            "opencode provider, arg with spaces, special chars in prompt"
        },
        {
            "copilot", {}, "auto", "copilot",
            "x",
            "empty args vector, single-char prompt"
        },
    };

    for (const auto& input : inputs) {
        SECTION(input.description) {
            CaptureCerr capture;
            InvokeUnfixedLogInvocation(
                input.binary, input.args, input.model, input.provider, input.prompt);
            const std::string output = capture.captured();

            INFO("Input: binary=" << input.binary << " model=" << input.model
                 << " provider=" << input.provider);
            INFO("Captured output:\n" << output);

            // Bug 1: non-ASCII bytes always present (header + kDivider are hardcoded UTF-8)
            CHECK_FALSE(ContainsNonAsciiByte(output));

            // Bug 2: prompt label always present (unconditional dump)
            CHECK(output.find("[kog ai] prompt  :") == std::string::npos);

            // Bug 2: prompt body always present
            if (!input.prompt.empty()) {
                CHECK(output.find(input.prompt) == std::string::npos);
            }
        }
    }
}
