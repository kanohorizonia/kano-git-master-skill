#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace kano::git::tests::functional {
namespace {

struct ScopedSandbox {
    SandboxContext sandbox;

    explicit ScopedSandbox(const std::string& InName)
        : sandbox(CreateSandboxWorkspace(InName)) {}
    ~ScopedSandbox() { RemoveSandboxWorkspace(sandbox); }
};

struct RepoFixture {
    std::filesystem::path path;
    std::string baseHead;
    std::string targetHead;
};

auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

auto WriteText(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::ofstream stream(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << InText;
    stream.close();
    REQUIRE(stream.good());
}

auto ReadText(const std::filesystem::path& InPath) -> std::string {
    std::ifstream stream(InPath, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

auto GitOutput(const std::filesystem::path& InRepo, const std::vector<std::string>& InArgs) -> std::string {
    const auto result = RunGit(InArgs, InRepo);
    RequireSuccess(result, "git fixture observation");
    return result.stdoutText;
}

auto InitRepo(const std::filesystem::path& InRepo) -> RepoFixture {
    std::filesystem::create_directories(InRepo);
    RequireSuccess(RunGit({"init", "-q"}, InRepo), "init fixture repo");
    RequireSuccess(RunGit({"config", "user.name", "KOG Test"}, InRepo), "configure fixture name");
    RequireSuccess(RunGit({"config", "user.email", "kog-test@example.invalid"}, InRepo), "configure fixture email");
    RequireSuccess(RunGit({"config", "core.autocrlf", "false"}, InRepo), "disable fixture newline conversion");
    RequireSuccess(RunGit({"config", "core.eol", "lf"}, InRepo), "pin fixture checkout newlines");
    RequireSuccess(RunGit({"checkout", "-b", "main"}, InRepo), "create fixture main");
    WriteText(InRepo / ".gitignore", ".kano/\n");
    WriteText(InRepo / "local-staged.txt", "base staged\n");
    WriteText(InRepo / "local-unstaged.txt", "base unstaged\n");
    RequireSuccess(RunGit({"add", ".gitignore", "local-staged.txt", "local-unstaged.txt"}, InRepo), "stage fixture base");
    RequireSuccess(RunGit({"commit", "-m", "seed fixture"}, InRepo), "commit fixture base");
    const auto baseHead = GitOutput(InRepo, {"rev-parse", "HEAD"});
    RequireSuccess(RunGit({"checkout", "-b", "upstream"}, InRepo), "create fixture upstream");
    WriteText(InRepo / "upstream.txt", "upstream change\n");
    RequireSuccess(RunGit({"add", "upstream.txt"}, InRepo), "stage upstream change");
    RequireSuccess(RunGit({"commit", "-m", "advance fixture upstream"}, InRepo), "commit upstream change");
    const auto targetHead = GitOutput(InRepo, {"rev-parse", "HEAD"});
    RequireSuccess(RunGit({"checkout", "main"}, InRepo), "restore fixture main");
    return {.path = InRepo, .baseHead = baseHead, .targetHead = targetHead};
}

auto ForeachArgs(const std::string& InTypes, const std::string& InParallel) -> std::vector<std::string> {
    return {
        "foreach", "--include-types", InTypes, "--parallel", InParallel, "--native-no-cache",
        "--command", "git merge --ff-only upstream && echo foreach-executed>foreach-marker.txt",
    };
}

auto RequireUnchanged(const RepoFixture& InRepo) -> void {
    REQUIRE(GitOutput(InRepo.path, {"rev-parse", "HEAD"}) == InRepo.baseHead);
    REQUIRE_FALSE(std::filesystem::exists(InRepo.path / "upstream.txt"));
    REQUIRE_FALSE(std::filesystem::exists(InRepo.path / "foreach-marker.txt"));
}

auto RequireExecuted(const RepoFixture& InRepo) -> void {
    REQUIRE(GitOutput(InRepo.path, {"rev-parse", "HEAD"}) == InRepo.targetHead);
    REQUIRE(ReadText(InRepo.path / "upstream.txt") == "upstream change\n");
    REQUIRE(ReadText(InRepo.path / "foreach-marker.txt").find("foreach-executed") != std::string::npos);
}

} // namespace

TEST_CASE("foreach dry-run preserves refs and local edits before real fast-forward execution",
          "[functional][KOG-BUG-0148][foreach][dry-run]") {
    const auto mode = GENERATE(std::string{}, std::string{"--native"}, std::string{"--native-plan"});
    INFO("mode=" << mode);
    const ScopedSandbox owner("foreach-dry-run-fast-forward");
    const auto repo = InitRepo(owner.sandbox.root / "repo");
    WriteText(repo.path / "local-staged.txt", "unrelated staged change\n");
    RequireSuccess(RunGit({"add", "local-staged.txt"}, repo.path), "stage unrelated edit");
    WriteText(repo.path / "local-unstaged.txt", "unrelated unstaged change\n");
    WriteText(repo.path / "local-untracked.txt", "unrelated untracked content\n");
    const auto statusBefore = GitOutput(repo.path, {"status", "--porcelain"});
    const auto indexBefore = GitOutput(repo.path, {"ls-files", "--stage", "local-staged.txt"});
    auto args = ForeachArgs("root", "1");
    if (!mode.empty()) args.push_back(mode);
    args.push_back("--dry-run");

    const auto preview = RunKog(args, repo.path);
    RequireSuccess(preview, "foreach dry-run");
    const auto plan = nlohmann::json::parse(preview.stdoutText);
    REQUIRE(plan.at("planner") == "native-foreach");
    REQUIRE(plan.at("operations").size() == 1);
    REQUIRE(plan.at("operations").at(0).at("type") == "root");
    REQUIRE(plan.at("operations").at(0).at("path") == repo.path.generic_string());
    RequireUnchanged(repo);
    REQUIRE(GitOutput(repo.path, {"status", "--porcelain"}) == statusBefore);
    REQUIRE(GitOutput(repo.path, {"ls-files", "--stage", "local-staged.txt"}) == indexBefore);

    args.back() = "--native-plan-only";
    const auto planOnly = RunKog(args, repo.path);
    RequireSuccess(planOnly, "foreach native-plan-only");
    REQUIRE(nlohmann::json::parse(planOnly.stdoutText) == plan);
    RequireUnchanged(repo);

    args.pop_back();
    RequireSuccess(RunKog(args, repo.path), "foreach real fast-forward");
    RequireExecuted(repo);
    REQUIRE(GitOutput(repo.path, {"ls-files", "--stage", "local-staged.txt"}) == indexBefore);
    REQUIRE(ReadText(repo.path / "local-staged.txt") == "unrelated staged change\n");
    REQUIRE(ReadText(repo.path / "local-unstaged.txt") == "unrelated unstaged change\n");
    REQUIRE(ReadText(repo.path / "local-untracked.txt") == "unrelated untracked content\n");
}

TEST_CASE("foreach dry-run preserves selected parallel repos and excludes the root",
          "[functional][KOG-BUG-0148][foreach][dry-run]") {
    const ScopedSandbox owner("foreach-dry-run-parallel");
    auto root = InitRepo(owner.sandbox.root / "repo");
    const auto first = InitRepo(root.path / "first");
    const auto second = InitRepo(root.path / "second");
    WriteText(root.path / ".gitmodules",
              "[submodule \"first\"]\n\tpath = first\n\turl = ./first\n"
              "[submodule \"second\"]\n\tpath = second\n\turl = ./second\n");
    RequireSuccess(RunGit({"add", ".gitmodules", "first", "second"}, root.path), "stage registered siblings");
    RequireSuccess(RunGit({"commit", "-m", "register fixture siblings"}, root.path), "register fixture siblings");
    root.baseHead = GitOutput(root.path, {"rev-parse", "HEAD"});
    auto args = ForeachArgs("submodule", "2");
    args.push_back("--dry-run");

    const auto preview = RunKog(args, root.path);
    RequireSuccess(preview, "foreach parallel dry-run");
    const auto plan = nlohmann::json::parse(preview.stdoutText);
    const auto& operations = plan.at("operations");
    REQUIRE(operations.size() == 2);
    REQUIRE(operations.at(0).at("wave") == operations.at(1).at("wave"));
    for (const auto& operation : operations) {
        REQUIRE(operation.at("type") == "registered");
        REQUIRE(operation.at("path") != root.path.generic_string());
    }
    RequireUnchanged(root);
    RequireUnchanged(first);
    RequireUnchanged(second);

    args.pop_back();
    RequireSuccess(RunKog(args, root.path), "foreach real parallel execution");
    RequireUnchanged(root);
    RequireExecuted(first);
    RequireExecuted(second);
}

} // namespace kano::git::tests::functional
