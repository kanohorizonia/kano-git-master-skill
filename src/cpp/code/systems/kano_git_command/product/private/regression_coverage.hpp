#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands::regression {

inline constexpr int kInvalidManifestExitCode = 2;
inline constexpr int kCoverageGapExitCode = 3;

struct RegressionCase {
  std::string caseId;
  std::string testName;
  std::string testFile;
  std::string testKind;
  std::string mappingState;
};

struct Incident {
  std::string incidentId;
  std::string incidentDate;
  std::string rootCause;
  std::string backlogRef;
  std::string changeRef;
  std::vector<std::string> workflowContracts;
  std::vector<RegressionCase> regressionCases;
};

struct CoverageReport {
  std::filesystem::path manifestPath;
  std::string manifestSchema;
  std::vector<Incident> incidents;
  std::vector<std::string> gaps;
};

struct LoadResult {
  bool ok = false;
  std::string error;
  CoverageReport report;
};

auto LoadCoverageManifest(const std::filesystem::path &InManifestPath)
    -> LoadResult;
auto RenderCoverageText(const CoverageReport &InReport) -> std::string;
auto RenderCoverageJson(const CoverageReport &InReport) -> std::string;
auto RenderCoverageErrorJson(const std::filesystem::path &InManifestPath,
                             const std::string &InError) -> std::string;
auto CoverageExitCode(const CoverageReport &InReport, bool InFailOnGap) -> int;

} // namespace kano::git::commands::regression
