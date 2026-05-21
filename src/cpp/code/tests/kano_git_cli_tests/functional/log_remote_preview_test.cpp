#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

struct RemoteCloneContext {
    SandboxContext sandbox;
    std::filesystem::path bareRemote;
    std::filesystem::path seedRepo;
    std::filesystem::path cloneRepo;
    std::string branch;
};

auto StripAnsi(const std::string& InText) -> std::string {
    std::string out;
    out.reserve(InText.size());
    bool inEsc = false;
    for (const char ch : InText) {
        if (!inEsc) {
            if (ch == '\x1b') {
                inEsc = true;
                continue;
            }
            out.push_back(ch);
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            inEsc = false;
        }
    }
    return out;
}

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto CreateRemoteWithClone(const std::string& InName, const std::string& InBranch = "main") -> RemoteCloneContext {
    RemoteCloneContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.bareRemote = (ctx.sandbox.root / "remote.git").lexically_normal();
    ctx.seedRepo = (ctx.sandbox.root / "seed").lexically_normal();
    ctx.cloneRepo = (ctx.sandbox.root / "clone").lexically_normal();
    ctx.branch = InBranch;

    RequireSuccess(RunGit({"init", "--bare", ctx.bareRemote.string()}, ctx.sandbox.root), "init bare remote");
    RequireSuccess(RunGit({"init", ctx.seedRepo.string()}, ctx.sandbox.root), "init seed repo");
    ConfigureIdentity(ctx.seedRepo);
    RequireSuccess(RunGit({"checkout", "-b", ctx.branch}, ctx.seedRepo), "checkout seed branch");
    WriteTextFile(ctx.seedRepo / "README.md", "seed\n");
    RequireSuccess(RunGit({"add", "README.md"}, ctx.seedRepo), "seed add");
    RequireSuccess(RunGit({"commit", "-m", "seed commit"}, ctx.seedRepo), "seed commit");
    RequireSuccess(RunGit({"remote", "add", "origin", ctx.bareRemote.string()}, ctx.seedRepo), "seed add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", ctx.branch}, ctx.seedRepo), "seed push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + ctx.branch)}, ctx.bareRemote), "set bare HEAD");
    RequireSuccess(RunGit({"clone", ctx.bareRemote.string(), ctx.cloneRepo.string()}, ctx.sandbox.root), "clone repo");
    ConfigureIdentity(ctx.cloneRepo);
    return ctx;
}

auto CountLinesWithToken(const std::string& InText, const std::string& InToken) -> int {
    int out = 0;
    std::istringstream iss(InText);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(InToken) != std::string::npos) {
            out += 1;
        }
    }
    return out;
}

void AddRemoteCommitsAndPush(const RemoteCloneContext& InCtx, int InCount) {
    for (int i = 0; i < InCount; ++i) {
        WriteTextFile(InCtx.seedRepo / "remote.txt", "remote commit " + std::to_string(i) + "\n");
        RequireSuccess(RunGit({"add", "remote.txt"}, InCtx.seedRepo), "seed add remote commit");
        RequireSuccess(RunGit({"commit", "-m", "remote advance " + std::to_string(i)}, InCtx.seedRepo), "seed commit remote advance");
    }
    RequireSuccess(RunGit({"push", "origin", InCtx.branch}, InCtx.seedRepo), "seed push remote advances");
    RequireSuccess(RunGit({"fetch", "origin", InCtx.branch}, InCtx.cloneRepo), "clone fetch remote advances");
}

} // namespace

TEST_CASE("log behind remote preview default three with ellipsis", "[functional][log][remote-preview]") {
    const auto ctx = CreateRemoteWithClone("log-behind-five");
    AddRemoteCommitsAndPush(ctx, 5);

    const auto result = RunKog({"log", "--no-recursive", "--remote-count", "3"}, ctx.cloneRepo);
    const auto plain = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(plain);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CountLinesWithToken(plain, "REMOTE [") == 3);
    REQUIRE(CountLinesWithToken(plain, "LOCAL [") == 1);
    REQUIRE(plain.find("... 2 remote commits omitted") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("log behind two has no ellipsis", "[functional][log][remote-preview]") {
    const auto ctx = CreateRemoteWithClone("log-behind-two");
    AddRemoteCommitsAndPush(ctx, 2);

    const auto result = RunKog({"log", "--no-recursive", "--remote-count", "3"}, ctx.cloneRepo);
    const auto plain = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(plain);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CountLinesWithToken(plain, "REMOTE [") == 2);
    REQUIRE(plain.find("... remote commits omitted") == std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("log remote count override and disable", "[functional][log][remote-preview]") {
    const auto ctx = CreateRemoteWithClone("log-remote-count-override");
    AddRemoteCommitsAndPush(ctx, 4);

    const auto countOne = RunKog({"log", "--no-recursive", "--remote-count", "1"}, ctx.cloneRepo);
    const auto onePlain = StripAnsi(countOne.stdoutText + "\n" + countOne.stderrText);
    INFO(onePlain);
    REQUIRE(countOne.exitCode == 0);
    REQUIRE(CountLinesWithToken(onePlain, "REMOTE [") == 1);
    REQUIRE(onePlain.find("... 3 remote commits omitted") != std::string::npos);

    const auto countZero = RunKog({"log", "--no-recursive", "--remote-count", "0"}, ctx.cloneRepo);
    const auto zeroPlain = StripAnsi(countZero.stdoutText + "\n" + countZero.stderrText);
    INFO(zeroPlain);
    REQUIRE(countZero.exitCode == 0);
    REQUIRE(CountLinesWithToken(zeroPlain, "REMOTE [") == 0);

    const auto disabled = RunKog({"log", "--no-recursive", "--no-remote-preview"}, ctx.cloneRepo);
    const auto disabledPlain = StripAnsi(disabled.stdoutText + "\n" + disabled.stderrText);
    INFO(disabledPlain);
    REQUIRE(disabled.exitCode == 0);
    REQUIRE(CountLinesWithToken(disabledPlain, "REMOTE [") == 0);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("log remote preview respects local config default", "[functional][log][remote-preview][config]") {
    const auto ctx = CreateRemoteWithClone("log-remote-config");
    AddRemoteCommitsAndPush(ctx, 3);
    WriteTextFile(ctx.cloneRepo / ".kano" / "kog_config.toml", "[log]\nremote_preview_count = 1\n");

    const auto result = RunKog({"log", "--no-recursive"}, ctx.cloneRepo);
    const auto plain = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(plain);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CountLinesWithToken(plain, "REMOTE [") == 1);
    REQUIRE(plain.find("... 2 remote commits omitted") != std::string::npos);
    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("slog diverged shows remote and local preview", "[functional][slog][remote-preview][diverged]") {
    const auto ctx = CreateRemoteWithClone("slog-diverged");

    WriteTextFile(ctx.cloneRepo / "local-only.txt", "local diverged commit\n");
    RequireSuccess(RunGit({"add", "local-only.txt"}, ctx.cloneRepo), "clone add local diverged");
    RequireSuccess(RunGit({"commit", "-m", "local diverged"}, ctx.cloneRepo), "clone commit local diverged");

    WriteTextFile(ctx.seedRepo / "remote-only.txt", "remote diverged commit\n");
    RequireSuccess(RunGit({"add", "remote-only.txt"}, ctx.seedRepo), "seed add remote diverged");
    RequireSuccess(RunGit({"commit", "-m", "remote diverged"}, ctx.seedRepo), "seed commit remote diverged");
    RequireSuccess(RunGit({"push", "origin", ctx.branch}, ctx.seedRepo), "seed push remote diverged");
    RequireSuccess(RunGit({"fetch", "origin", ctx.branch}, ctx.cloneRepo), "clone fetch remote diverged");

    const auto result = RunKog({"slog", "--no-recursive", "--remote-count", "1"}, ctx.cloneRepo);
    const auto plain = StripAnsi(result.stdoutText + "\n" + result.stderrText);
    INFO(plain);
    REQUIRE(result.exitCode == 0);
    REQUIRE(CountLinesWithToken(plain, "REMOTE [") >= 1);
    REQUIRE(CountLinesWithToken(plain, "LOCAL [") >= 1);
    RemoveSandboxWorkspace(ctx.sandbox);
}

} // namespace kano::git::tests::functional
