#include <CLI/CLI.hpp>

#include "plan_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace kano::git::commands {

void RegisterIgnore(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("ignore", "Ignore management commands");
    auto* doctor = cmd->add_subcommand("doctor", "Scan tracked files for likely ignore candidates");
    auto* repo = new std::string{};
    auto* limit = new int{200};
    auto* asJson = new bool{false};
    auto* apply = new bool{false};
    auto* dryRun = new bool{false};
    auto* yes = new bool{false};
    doctor->add_option("--repo", *repo, "Workspace/repo root path");
    doctor->add_option("--limit", *limit, "Max findings")->default_val(200);
    doctor->add_flag("--json", *asJson, "Output JSON");
    doctor->add_flag("--apply", *apply, "Apply untrack (git rm --cached) to findings");
    doctor->add_flag("--dry-run", *dryRun, "Print apply actions only");
    doctor->add_flag("--yes,-y", *yes, "Skip interactive confirmation gate");
    doctor->callback([=]() {
        const auto root = repo->empty() ? std::filesystem::current_path().lexically_normal() : std::filesystem::path(*repo).lexically_normal();
        std::exit(RunIgnoreDoctor(root, *limit, *asJson, *apply, *dryRun, *yes));
    });
}

} // namespace kano::git::commands
