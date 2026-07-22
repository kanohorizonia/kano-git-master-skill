// Thin CLI registration boundary for the auth command.

#include <CLI/CLI.hpp>

#include "auth_cmd.hpp"

#include <memory>

namespace kano::git::commands {

void RegisterAuth(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("auth", "Credential manager diagnostics and non-interactive auth probes");
    const auto options = std::make_shared<AuthCommandOptions>();

    auto* doctor = cmd->add_subcommand("doctor", "Inspect Git Credential Manager and selected remote auth configuration");
    doctor->add_option("--repo", options->doctor.repo, "Repository root used for config inspection and target discovery");
    doctor->add_option("--remote", options->doctor.remote, "Inspect a single configured remote in the current repository");
    doctor->add_option("--url", options->doctor.url, "Inspect an explicit remote URL without storing credentials");
    doctor->add_flag("--selected-remotes", options->doctor.selectedRemotes, "Inspect the sync-selected remote for each discovered repo");
    doctor->add_flag("--all-local-remotes", options->doctor.allLocalRemotes, "Inspect every configured remote in the current repository");
    doctor->add_flag("--no-recursive,-N", options->doctor.noRecursive, "When used with --selected-remotes, inspect only the current repository");
    doctor->add_flag("--native-no-cache", options->doctor.noCache, "Disable native discovery cache for --selected-remotes");
    doctor->add_flag("--native-refresh-cache", options->doctor.refreshCache, "Force native discovery cache refresh for --selected-remotes");
    doctor->add_flag("--fix", options->doctor.fix, "Remove stale credential.helper=manager-core entries and configure modern Git Credential Manager if needed");
    doctor->callback(MakeAuthDoctorCommandCallback(options));

    auto* test = cmd->add_subcommand("test", "Run a non-interactive git ls-remote auth probe");
    test->add_option("--repo", options->test.repo, "Repository root used for target discovery and ls-remote working directory");
    test->add_option("--remote", options->test.remote, "Probe one configured remote in the current repository");
    test->add_option("--url", options->test.url, "Probe an explicit remote URL without storing credentials");
    test->add_flag("--selected-remotes", options->test.selectedRemotes, "Probe the sync-selected remote for each discovered repo");
    test->add_flag("--all-local-remotes", options->test.allLocalRemotes, "Probe every configured remote in the current repository");
    test->add_flag("--no-recursive,-N", options->test.noRecursive, "When used with --selected-remotes, probe only the current repository");
    test->add_flag("--native-no-cache", options->test.noCache, "Disable native discovery cache for --selected-remotes");
    test->add_flag("--native-refresh-cache", options->test.refreshCache, "Force native discovery cache refresh for --selected-remotes");
    test->callback(MakeAuthTestCommandCallback(options));
}

} // namespace kano::git::commands
