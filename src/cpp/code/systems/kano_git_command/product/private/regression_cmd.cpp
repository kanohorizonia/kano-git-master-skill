#include <CLI/CLI.hpp>

#include "regression_coverage.hpp"
#include "runtime_path_layout.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace kano::git::commands {

void RegisterRegression(CLI::App &InApp) {
  auto *regressionCmd = InApp.add_subcommand(
      "regression", "Inspect dogfood incident-to-regression traceability");
  regressionCmd->require_subcommand(1);

  auto *coverageCmd = regressionCmd->add_subcommand(
      "coverage", "Report linked regression cases and coverage gaps");
  auto *manifestPath = new std::string{};
  auto *outputFormat = new std::string{"text"};
  auto *jsonOutput = new bool{false};
  auto *failOnGap = new bool{false};
  coverageCmd->add_option("--manifest", *manifestPath,
                          "Override the incident mapping manifest path");
  coverageCmd->add_option("--format", *outputFormat,
                          "Output format: text or json");
  coverageCmd->add_flag("--json", *jsonOutput, "Alias for --format json");
  coverageCmd->add_flag("--fail-on-gap", *failOnGap,
                        "Exit 3 when a valid manifest contains an incident "
                        "without a linked case");
  coverageCmd->callback([=]() {
    if (*outputFormat != "text" && *outputFormat != "json") {
      std::cerr << "kog regression coverage: invalid --format '"
                << *outputFormat << "' (expected text or json)\n";
      throw CLI::RuntimeError(regression::kInvalidManifestExitCode);
    }
    const bool useJson = *jsonOutput || *outputFormat == "json";
    const auto resolvedManifest =
        manifestPath->empty()
            ? runtime_path::Layout::Resolve(std::filesystem::current_path())
                  .RegressionIncidentManifest()
            : std::filesystem::path(*manifestPath).lexically_normal();
    const auto loaded = regression::LoadCoverageManifest(resolvedManifest);
    if (!loaded.ok) {
      if (useJson) {
        std::cout << regression::RenderCoverageErrorJson(resolvedManifest,
                                                         loaded.error);
      } else {
        std::cerr << "kog regression coverage: " << loaded.error << "\n";
      }
      throw CLI::RuntimeError(regression::kInvalidManifestExitCode);
    }

    if (useJson) {
      std::cout << regression::RenderCoverageJson(loaded.report);
    } else {
      std::cout << regression::RenderCoverageText(loaded.report);
    }
    const int exitCode =
        regression::CoverageExitCode(loaded.report, *failOnGap);
    if (exitCode != 0) {
      throw CLI::RuntimeError(exitCode);
    }
  });
}

} // namespace kano::git::commands
