#include "commit_plan_path_scope.hpp"

#include <algorithm>
#include <utility>

namespace kano::git::commands {
namespace {

auto TrimPathspec(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' ||
            InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() &&
           (InValue[start] == ' ' || InValue[start] == '\t')) {
        ++start;
    }
    return InValue.substr(start);
}

} // namespace

auto NormalizePlanPathspecForSafety(std::string InPath) -> std::string {
    std::replace(InPath.begin(), InPath.end(), '\\', '/');
    while (InPath.rfind("./", 0) == 0) InPath.erase(0, 2);
    return TrimPathspec(std::move(InPath));
}

auto NormalizeGitReportedPathForPlanSafety(std::string InPath) -> std::string {
    // Git's -z output is exact and unquoted. Whitespace and backslash remain
    // meaningful filename bytes on POSIX and must not be normalized away.
    while (InPath.rfind("./", 0) == 0) InPath.erase(0, 2);
    return InPath;
}

auto PlanPathspecCoversPath(std::string InPathspec, std::string InPath) -> bool {
    InPathspec = NormalizePlanPathspecForSafety(std::move(InPathspec));
    InPath = NormalizeGitReportedPathForPlanSafety(std::move(InPath));
    if (InPathspec.empty() || InPath.empty()) return false;
    if (InPathspec == InPath) return true;
    if (InPathspec.back() != '/') InPathspec.push_back('/');
    return InPath.rfind(InPathspec, 0) == 0;
}

} // namespace kano::git::commands
