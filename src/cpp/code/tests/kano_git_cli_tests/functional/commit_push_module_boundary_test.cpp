#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto ReadFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("commit-push registration stays behind a typed CLI boundary",
          "[architecture][commit-push][module-boundary][KG-TSK-0115]") {
    const auto cppRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot =
        cppRoot / "code" / "systems" / "kano_git_command" / "commit_plan";
    const auto commandSource = ReadFile(moduleRoot / "private" / "commit_push_cmd.cpp");
    const auto cliSource = ReadFile(moduleRoot / "private" / "commit_push_cli.cpp");
    const auto commandHeader = ReadFile(moduleRoot / "private" / "commit_push_cmd.hpp");
    const auto moduleCMake = ReadFile(moduleRoot / "CMakeLists.txt");

    REQUIRE(commandSource.find("add_subcommand(\"commit-push\"") == std::string::npos);
    REQUIRE(commandSource.find("cmd->callback(") == std::string::npos);
    REQUIRE(commandSource.find("new std::string") == std::string::npos);
    REQUIRE(cliSource.find("add_subcommand(") != std::string::npos);
    REQUIRE(cliSource.find("std::make_shared<CommitPushCommandOptions>") != std::string::npos);
    REQUIRE(cliSource.find("MakeCommitPushCommandCallback(*cmd, options)") != std::string::npos);
    REQUIRE(commandHeader.find("struct CommitPushCommandOptions") != std::string::npos);
    REQUIRE(commandSource.find("optionsOwner = InOptions") != std::string::npos);
    REQUIRE(moduleCMake.find("private/commit_push_cli.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
