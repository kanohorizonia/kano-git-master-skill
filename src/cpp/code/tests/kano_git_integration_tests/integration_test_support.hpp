#pragma once

#include "functional_test_support.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace kano::git::tests::integration {

using functional::CommandResult;
using functional::SandboxContext;

struct RemoteCloneContext {
    SandboxContext sandbox;
    std::filesystem::path bareRemote;
    std::filesystem::path seedRepo;
    std::filesystem::path cloneRepo;
    std::string branch;
};

auto CreateRemoteWithClone(const std::string& InName,
                           const std::string& InBranch = "main") -> RemoteCloneContext;
auto RemoveRemoteContext(const RemoteCloneContext& InContext) -> void;
auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void;
auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void;
auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void;
auto RequireFailure(const CommandResult& InResult, const std::string& InContext) -> void;
auto CurrentHeadSha(const std::filesystem::path& InRepo) -> std::string;
auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string;
auto RefSha(const std::filesystem::path& InRepo, const std::string& InRef) -> std::string;
auto AheadBehindCounts(const std::filesystem::path& InRepo) -> std::pair<int, int>;
auto StatusPorcelain(const std::filesystem::path& InRepo) -> std::string;
auto TrimCopy(const std::string& InValue) -> std::string;

} // namespace kano::git::tests::integration
