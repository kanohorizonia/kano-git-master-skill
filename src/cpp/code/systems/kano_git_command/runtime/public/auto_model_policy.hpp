#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands::auto_model_policy {

struct AutoModelPolicy {
    std::vector<int> changeThresholds = {5, 10};
    std::vector<std::string> models = {"gpt-5-mini", "claude-haiku-4.5", "gpt-5.4"};
};

auto ResolveAutoModelPolicy(const std::string& InProvider,
                            const std::filesystem::path& InWorkspaceRoot,
                            const std::filesystem::path& InSkillRoot) -> AutoModelPolicy;

auto ResolveModelForChangeCount(const AutoModelPolicy& InPolicy, int InChangeCount) -> std::string;

} // namespace kano::git::commands::auto_model_policy
