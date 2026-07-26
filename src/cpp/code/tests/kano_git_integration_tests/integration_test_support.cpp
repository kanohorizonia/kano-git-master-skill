#include "integration_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

namespace kano::git::tests::integration {

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto RequireFailure(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode != 0);
}

auto TrimCopy(const std::string& InValue) -> std::string {
    const auto start = InValue.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = InValue.find_last_not_of(" \t\r\n");
    return InValue.substr(start, end - start + 1);
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    const auto hooksDir = (InRepo / ".git-hooks-disabled").lexically_normal();
    std::filesystem::create_directories(hooksDir);
    RequireSuccess(functional::RunGit({"config", "user.name", "Kano Integration"}, InRepo),
                   "configure integration user.name");
    RequireSuccess(functional::RunGit({"config", "user.email", "kano-integration@example.invalid"}, InRepo),
                   "configure integration user.email");
    RequireSuccess(functional::RunGit({"config", "core.hooksPath", hooksDir.string()}, InRepo),
                   "disable fixture hooks");
}

auto CreateRemoteWithClone(const std::string& InName,
                           const std::string& InBranch) -> RemoteCloneContext {
    RemoteCloneContext ctx;
    ctx.sandbox = functional::CreateSandboxWorkspace("integration-" + InName);
    ctx.bareRemote = (ctx.sandbox.root / "remote.git").lexically_normal();
    ctx.seedRepo = (ctx.sandbox.root / "seed").lexically_normal();
    ctx.cloneRepo = (ctx.sandbox.root / "clone").lexically_normal();
    ctx.branch = InBranch;

    RequireSuccess(functional::RunGit({"init", "--bare", ctx.bareRemote.string()}, ctx.sandbox.root),
                   "initialize bare remote");
    RequireSuccess(functional::RunGit({"init", ctx.seedRepo.string()}, ctx.sandbox.root),
                   "initialize seed repository");
    ConfigureIdentity(ctx.seedRepo);
    RequireSuccess(functional::RunGit({"checkout", "-b", ctx.branch}, ctx.seedRepo),
                   "create seed branch");
    WriteTextFile(ctx.seedRepo / ".gitignore", ".kano/\n.git-hooks-disabled/\n");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    RequireSuccess(functional::RunGit({"add", ".gitignore", "README.md"}, ctx.seedRepo),
                   "stage seed files");
    RequireSuccess(functional::RunGit({"commit", "-m", "test(integration): seed remote"}, ctx.seedRepo),
                   "commit seed files");
    RequireSuccess(functional::RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo),
                   "add seed remote");
    RequireSuccess(functional::RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo),
                   "publish seed branch");
    RequireSuccess(functional::RunGit({"symbolic-ref", "HEAD", "refs/heads/" + ctx.branch}, ctx.bareRemote),
                   "set bare remote HEAD");
    RequireSuccess(functional::RunGit({"clone", ctx.bareRemote.string(), ctx.cloneRepo.string()}, ctx.sandbox.root),
                   "clone integration repository");
    ConfigureIdentity(ctx.cloneRepo);
    RequireSuccess(
        functional::RunGit({"config", "kano.cache.local-dir", (ctx.sandbox.root / "_cache").string()}, ctx.cloneRepo),
        "configure external KOG cache");
    return ctx;
}

auto RemoveRemoteContext(const RemoteCloneContext& InContext) -> void {
    functional::RemoveSandboxWorkspace(InContext.sandbox);
}

auto CurrentHeadSha(const std::filesystem::path& InRepo) -> std::string {
    const auto result = functional::RunGit({"rev-parse", "HEAD"}, InRepo);
    RequireSuccess(result, "resolve HEAD");
    return TrimCopy(result.stdoutText);
}

auto CurrentBranch(const std::filesystem::path& InRepo) -> std::string {
    const auto result = functional::RunGit({"symbolic-ref", "--quiet", "--short", "HEAD"}, InRepo);
    if (result.exitCode != 0) {
        return {};
    }
    return TrimCopy(result.stdoutText);
}

auto RefSha(const std::filesystem::path& InRepo, const std::string& InRef) -> std::string {
    const auto result = functional::RunGit({"rev-parse", InRef}, InRepo);
    RequireSuccess(result, "resolve ref " + InRef);
    return TrimCopy(result.stdoutText);
}

auto AheadBehindCounts(const std::filesystem::path& InRepo) -> std::pair<int, int> {
    const auto result = functional::RunGit({"rev-list", "--left-right", "--count", "@{upstream}...HEAD"}, InRepo);
    RequireSuccess(result, "resolve ahead/behind counts");
    std::istringstream values(result.stdoutText);
    int behind = 0;
    int ahead = 0;
    values >> behind >> ahead;
    return {behind, ahead};
}

auto StatusPorcelain(const std::filesystem::path& InRepo) -> std::string {
    const auto result = functional::RunGit({"status", "--porcelain"}, InRepo);
    RequireSuccess(result, "read porcelain status");
    return TrimCopy(result.stdoutText);
}

} // namespace kano::git::tests::integration
