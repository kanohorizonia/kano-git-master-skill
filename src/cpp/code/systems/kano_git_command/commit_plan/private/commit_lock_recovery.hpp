#pragma once

#include "commit_ai_utils.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace kano::git::commands {

struct CommitLockRecoveryState {
    std::mutex mutex;
    bool attempted = false;
    bool succeeded = false;
    std::string reason;
};

auto MaybeRecoverCommitLockFailure(
    const std::filesystem::path& InWorkspaceRoot,
    RepoCommitResult InResult,
    CommitLockRecoveryState& InState,
    const std::function<RepoCommitResult()>& InRetryFn) -> RepoCommitResult;

} // namespace kano::git::commands
