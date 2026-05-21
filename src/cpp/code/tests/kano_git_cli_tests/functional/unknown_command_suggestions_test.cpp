#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace

TEST_CASE("unknown command suggests log", "[functional][cli][unknown-command]") {
    const auto sandbox = CreateSandboxWorkspace("unknown-command-log");
    const auto result = RunKog({"lgo"}, sandbox.root);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("kog: 'lgo' is not a kog command. See 'kog --help'.") != std::string::npos);
    REQUIRE(merged.find("The most similar command is") != std::string::npos);
    REQUIRE(merged.find("        log") != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("unknown command suggests status", "[functional][cli][unknown-command]") {
    const auto sandbox = CreateSandboxWorkspace("unknown-command-status");
    const auto result = RunKog({"statsu"}, sandbox.root);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("kog: 'statsu' is not a kog command. See 'kog --help'.") != std::string::npos);
    REQUIRE(merged.find("        status") != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("unknown command suggests fetch", "[functional][cli][unknown-command][fetch]") {
    const auto sandbox = CreateSandboxWorkspace("unknown-command-fetch");
    const auto result = RunKog({"fetc"}, sandbox.root);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("kog: 'fetc' is not a kog command. See 'kog --help'.") != std::string::npos);
    REQUIRE(merged.find("        fetch") != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("far unknown command has no suggestion", "[functional][cli][unknown-command]") {
    const auto sandbox = CreateSandboxWorkspace("unknown-command-far");
    const auto result = RunKog({"totally-not-a-command"}, sandbox.root);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("kog: 'totally-not-a-command' is not a kog command. See 'kog --help'.") != std::string::npos);
    REQUIRE(merged.find("The most similar command") == std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("known command bad option keeps parse error path", "[functional][cli][unknown-command]") {
    const auto sandbox = CreateSandboxWorkspace("known-command-bad-option");
    const auto result = RunKog({"status", "--definitely-bad-option"}, sandbox.root);
    const auto merged = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(merged);
    REQUIRE(result.exitCode != 0);
    REQUIRE(merged.find("is not a kog command") == std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
