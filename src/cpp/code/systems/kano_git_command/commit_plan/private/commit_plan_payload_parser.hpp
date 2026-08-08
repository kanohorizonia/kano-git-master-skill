#pragma once

#include "commit_plan_payload.hpp"

#include <optional>
#include <string>

namespace kano::git::commands {

auto ParseCommitPlanText(const std::string& InText,
                         std::string* OutError)
    -> std::optional<CommitPlanPayload>;

} // namespace kano::git::commands
