#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kano::git::tests::functional {

TEST_CASE("preset artifact identity survives automatic CMake cache reset",
          "[architecture][build][preset-identity][KG-BUG-0068]") {
    const auto cppRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
    std::ifstream input(cppRoot / "CMakeLists.txt", std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto cmake = buffer.str();

    const auto fallback = cmake.find("$ENV{KANO_CPP_INFRA_BUILD_CONFIGURE_PRESET}");
    const auto metadata = cmake.find("set(KOG_BUILD_PRESET_STR \"${KOG_PRESET_NAME}\")");
    REQUIRE(fallback != std::string::npos);
    REQUIRE(metadata != std::string::npos);
    REQUIRE(fallback < metadata);
    const auto explicitPresetGuard = cmake.find("NOT DEFINED KOG_PRESET_NAME");
    REQUIRE(explicitPresetGuard != std::string::npos);
    REQUIRE(explicitPresetGuard < fallback);
}

} // namespace kano::git::tests::functional
