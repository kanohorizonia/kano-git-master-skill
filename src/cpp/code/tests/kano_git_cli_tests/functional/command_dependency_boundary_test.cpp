#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {
namespace {

auto ReadDependencyFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

TEST_CASE("command libraries form an acyclic registration composition graph",
          "[architecture][command][dependency-boundary]") {
    const auto codeRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto coreRoot = codeRoot / "systems/kano_git_core";
    const auto commandRoot = codeRoot / "systems/kano_git_command";

    const auto coreCmake = ReadDependencyFile(coreRoot / "CMakeLists.txt");
    const auto configCmake = ReadDependencyFile(coreRoot / "config/CMakeLists.txt");
    const auto workspaceCmake = ReadDependencyFile(coreRoot / "workspace/CMakeLists.txt");
    const auto commandCmake = ReadDependencyFile(commandRoot / "CMakeLists.txt");
    const auto runtimeCmake = ReadDependencyFile(commandRoot / "runtime/CMakeLists.txt");
    const auto registrationCmake = ReadDependencyFile(commandRoot / "registration/CMakeLists.txt");

    REQUIRE(coreCmake.find("add_subdirectory(config)") != std::string::npos);
    REQUIRE(configCmake.find("add_library(kano_git_config STATIC)") != std::string::npos);
    REQUIRE(configCmake.find("private/kog_config.cpp") != std::string::npos);
    REQUIRE(configCmake.find("public/kog_config.hpp") != std::string::npos);

    REQUIRE(workspaceCmake.find("kano::git_config") != std::string::npos);
    REQUIRE(workspaceCmake.find("kano::git_command_runtime") == std::string::npos);

    REQUIRE(runtimeCmake.find("kano::git_config") != std::string::npos);
    REQUIRE(runtimeCmake.find("command_registry.cpp") == std::string::npos);
    REQUIRE(runtimeCmake.find("command_declarations.hpp") == std::string::npos);
    REQUIRE(runtimeCmake.find("private/kog_config.cpp") == std::string::npos);
    REQUIRE(runtimeCmake.find("public/kog_config.hpp") == std::string::npos);

    REQUIRE(commandCmake.find("add_subdirectory(registration)") != std::string::npos);
    REQUIRE(commandCmake.find("kano::git_command_registration") != std::string::npos);
    REQUIRE(commandCmake.find("--start-group") == std::string::npos);
    REQUIRE(commandCmake.find("--end-group") == std::string::npos);

    REQUIRE(registrationCmake.find("add_library(kano_git_command_registration STATIC)") != std::string::npos);
    REQUIRE(registrationCmake.find("private/command_registry.cpp") != std::string::npos);
    REQUIRE(registrationCmake.find("private/command_declarations.hpp") != std::string::npos);
    REQUIRE(registrationCmake.find("public/command_registry.hpp") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_command_runtime") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_cmd_repo_sync") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_cmd_commit_plan") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_cmd_ui") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_cmd_bridge") != std::string::npos);
    REQUIRE(registrationCmake.find("kano::git_cmd_product") != std::string::npos);
}

} // namespace kano::git::tests::functional
