// version_command_surface_test.cpp
// Tests: kog version, kog version --verbose, kog self status

#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>

namespace kano::git::tests::functional {

TEST_CASE("kog_version_emits_revision_aware_version", "[functional][cli][version]") {
    const auto sandbox = CreateSandboxWorkspace("version-basic");
    const auto result  = RunKog({"version"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto out = result.stdoutText;
    REQUIRE_FALSE(out.empty());
    // Must not be a plain workspace table
    REQUIRE(out.find("Version:  ") == std::string::npos);
    // Must match revision-aware pattern: N.N.N.N[-suffix]
    // (The exact revision number varies, but must have 4 numeric components
    //  when base VERSION has 3, e.g. "0.1.0.<rev>-beta")
    static const std::regex versionPattern(R"([0-9]+\.[0-9]+(\.[0-9]+)*(\-[a-zA-Z0-9\.\-]+)?)");
    REQUIRE(std::regex_search(out, versionPattern));
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog_version_verbose_emits_build_metadata", "[functional][cli][version]") {
    const auto sandbox = CreateSandboxWorkspace("version-verbose");
    const auto result  = RunKog({"version", "--verbose"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    const auto out = result.stdoutText;
    REQUIRE(out.find("version=") != std::string::npos);
    REQUIRE(out.find("branch=")  != std::string::npos);
    REQUIRE(out.find("rev=")     != std::string::npos);
    REQUIRE(out.find("hash_short=") != std::string::npos);
    REQUIRE(out.find("hash=")    != std::string::npos);
    REQUIRE(out.find("dirty=")   != std::string::npos);
    REQUIRE(out.find("host=")    != std::string::npos);
    REQUIRE(out.find("host_platform=")  != std::string::npos);
    REQUIRE(out.find("toolchain=")      != std::string::npos);
    REQUIRE(out.find("generator=")      != std::string::npos);
    REQUIRE(out.find("preset=")         != std::string::npos);
    REQUIRE(out.find("ci=")             != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog_version_verbose_vcs_is_git_in_git_checkout", "[functional][cli][version][vcs]") {
    const auto sandbox = CreateSandboxWorkspace("version-vcs-git");
    const auto result  = RunKog({"version", "--verbose"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    // In a normal git checkout build, vcs must not be "unknown"
    REQUIRE(result.stdoutText.find("vcs=git") != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog_version_verbose_rev_is_numeric", "[functional][cli][version][rev]") {
    const auto sandbox = CreateSandboxWorkspace("version-rev-numeric");
    const auto result  = RunKog({"version", "--verbose"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    // rev= must be followed by a number, not "unknown"
    static const std::regex revPattern(R"(rev=[0-9]+)");
    REQUIRE(std::regex_search(result.stdoutText, revPattern));
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog_self_status_exits_zero", "[functional][cli][self][status]") {
    const auto sandbox = CreateSandboxWorkspace("self-status-basic");
    const auto result  = RunKog({"self", "status"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("binary_version:") != std::string::npos);
    REQUIRE(result.stdoutText.find("packaged:")       != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog_self_status_json_is_valid_object", "[functional][cli][self][status]") {
    const auto sandbox = CreateSandboxWorkspace("self-status-json");
    const auto result  = RunKog({"self", "status", "--json"}, sandbox.root);
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("{")                 != std::string::npos);
    REQUIRE(result.stdoutText.find("}")                 != std::string::npos);
    REQUIRE(result.stdoutText.find("\"binary_version\"") != std::string::npos);
    REQUIRE(result.stdoutText.find("\"packaged\"")       != std::string::npos);
    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional