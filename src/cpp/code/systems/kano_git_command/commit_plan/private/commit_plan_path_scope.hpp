#pragma once

#include <string>

namespace kano::git::commands {

auto NormalizePlanPathspecForSafety(std::string InPath) -> std::string;
auto NormalizeGitReportedPathForPlanSafety(std::string InPath) -> std::string;
auto PlanPathspecCoversPath(std::string InPathspec, std::string InPath) -> bool;

} // namespace kano::git::commands
