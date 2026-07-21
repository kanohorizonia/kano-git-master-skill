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

TEST_CASE("repo namespace is public and provides native single-repo status", "[functional][cli][repo-target][KG-TSK-0061]") {
    const auto sandbox = CreateSandboxWorkspace("public-repo-namespace");
    const auto initResult = RunGit({"init"}, sandbox.root);
    INFO(initResult.stdoutText);
    INFO(initResult.stderrText);
    REQUIRE(initResult.exitCode == 0);

    const auto topLevelHelp = RunKog({"--help"}, sandbox.root);
    const auto topLevelMerged = StripAnsi(topLevelHelp.stdoutText + "\n" + topLevelHelp.stderrText);
    INFO(topLevelMerged);
    REQUIRE(topLevelHelp.exitCode == 0);
    REQUIRE(topLevelMerged.find("repo") != std::string::npos);
    REQUIRE(topLevelMerged.find("Single-repo scoped command variants") != std::string::npos);

    const auto repoHelp = RunKog({"repo", "--help"}, sandbox.root);
    const auto repoHelpMerged = StripAnsi(repoHelp.stdoutText + "\n" + repoHelp.stderrText);
    INFO(repoHelpMerged);
    REQUIRE(repoHelp.exitCode == 0);
    REQUIRE(repoHelpMerged.find("Single-repo scoped command variants") != std::string::npos);
    REQUIRE(repoHelpMerged.find("status") != std::string::npos);
    REQUIRE(repoHelpMerged.find("commit-push") != std::string::npos);
    REQUIRE(repoHelpMerged.find("is not a kog command") == std::string::npos);

    const auto repoStatus = RunKog({"repo", "status", ".", "--format", "json"}, sandbox.root);
    const auto repoStatusMerged = StripAnsi(repoStatus.stdoutText + "\n" + repoStatus.stderrText);
    INFO(repoStatusMerged);
    REQUIRE(repoStatus.exitCode == 0);
    REQUIRE(repoStatusMerged.find("\"repo_count\":1") != std::string::npos);

    const auto repoPush = RunKog({"repo", "push", ".", "--dry-run"}, sandbox.root);
    const auto repoPushMerged = StripAnsi(repoPush.stdoutText + "\n" + repoPush.stderrText);
    INFO(repoPushMerged);
    REQUIRE(repoPush.exitCode != 109);
    REQUIRE(repoPushMerged.find("argument was not expected") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("legacy workspace namespace is absent and split commands remain public", "[functional][cli][command-surface][KG-TSK-0062]") {
    const auto sandbox = CreateSandboxWorkspace("workspace-command-removed");

    const auto topLevelHelp = RunKog({"--help"}, sandbox.root);
    const auto topLevelMerged = StripAnsi(topLevelHelp.stdoutText + "\n" + topLevelHelp.stderrText);
    INFO(topLevelMerged);
    REQUIRE(topLevelHelp.exitCode == 0);
    REQUIRE(topLevelMerged.find("Workspace operations") == std::string::npos);

    for (const std::string command : {"discover", "foreach", "update"}) {
        const auto commandHelp = RunKog({command, "--help"}, sandbox.root);
        const auto commandHelpMerged = StripAnsi(commandHelp.stdoutText + "\n" + commandHelp.stderrText);
        INFO(command);
        INFO(commandHelpMerged);
        REQUIRE(commandHelp.exitCode == 0);
        REQUIRE(commandHelpMerged.find("is not a kog command") == std::string::npos);
    }

    const auto legacyHelp = RunKog({"workspace", "--help"}, sandbox.root);
    const auto legacyMerged = StripAnsi(legacyHelp.stdoutText + "\n" + legacyHelp.stderrText);
    INFO(legacyMerged);
    REQUIRE(legacyHelp.exitCode != 0);
    REQUIRE(legacyMerged.find("kog: 'workspace' is not a kog command") != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
