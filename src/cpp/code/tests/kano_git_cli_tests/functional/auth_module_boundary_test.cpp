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

auto CountOccurrences(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = InText.find(InNeedle, offset)) != std::string::npos) {
        ++count;
        offset += InNeedle.size();
    }
    return count;
}

} // namespace

TEST_CASE("auth registration stays behind a typed CLI boundary",
          "[architecture][auth][module-boundary][KG-TSK-0118]") {
    const auto cppRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto moduleRoot =
        cppRoot / "code" / "systems" / "kano_git_command" / "repo_sync";
    const auto commandSource = ReadFile(moduleRoot / "private" / "sync_cmd.cpp");
    const auto cliSource = ReadFile(moduleRoot / "private" / "auth_cli.cpp");
    const auto commandHeader = ReadFile(moduleRoot / "private" / "auth_cmd.hpp");
    const auto moduleCMake = ReadFile(moduleRoot / "CMakeLists.txt");

    const auto runtimeStart = commandSource.find("auto MakeAuthDoctorCommandCallback(");
    const auto runtimeEnd = commandSource.find("auto MakeSyncCommandOptions()");
    REQUIRE(runtimeStart != std::string::npos);
    REQUIRE(runtimeEnd != std::string::npos);
    REQUIRE(runtimeStart < runtimeEnd);
    const auto authRuntime = commandSource.substr(runtimeStart, runtimeEnd - runtimeStart);

    REQUIRE(commandSource.find("void RegisterAuth(") == std::string::npos);
    REQUIRE(authRuntime.find("add_subcommand(") == std::string::npos);
    REQUIRE(authRuntime.find("new std::string") == std::string::npos);
    REQUIRE(authRuntime.find("new bool") == std::string::npos);
    REQUIRE(cliSource.find("void RegisterAuth(") != std::string::npos);
    REQUIRE(cliSource.find("std::make_shared<AuthCommandOptions>") != std::string::npos);
    REQUIRE(cliSource.find("MakeAuthDoctorCommandCallback(options)") != std::string::npos);
    REQUIRE(cliSource.find("MakeAuthTestCommandCallback(options)") != std::string::npos);
    REQUIRE(commandHeader.find("struct AuthCommandOptions") != std::string::npos);
    REQUIRE(CountOccurrences(authRuntime, "optionsOwner = InOptions") == 2);
    REQUIRE(moduleCMake.find("private/auth_cli.cpp") != std::string::npos);
}

} // namespace kano::git::tests::functional
