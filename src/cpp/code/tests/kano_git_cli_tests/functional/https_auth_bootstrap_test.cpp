#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace kano::git::tests::functional {
namespace {

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream input(InPath, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

auto CreateFakeGcm(const std::filesystem::path& InPath) -> void {
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
    output << "@echo off\r\n"
              "echo 2.9.1-test\r\n"
              "exit /b 0\r\n";
#else
    output << "#!/usr/bin/env sh\n"
              "printf '%s\\n' '2.9.1-test'\n";
#endif
    output.close();
    REQUIRE(output.good());
#if !defined(_WIN32)
    std::filesystem::permissions(
        InPath,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
#endif
}

auto HttpsAuthEnvironment(const std::filesystem::path& InHome)
    -> std::vector<std::pair<std::string, std::string>> {
    return {
        {"HOME", InHome.string()},
        {"USERPROFILE", InHome.string()},
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", (InHome / "global.gitconfig").string()},
        {"KOG_TEST_MODE", "1"},
    };
}

} // namespace

TEST_CASE("https auth setup dry-run previews scoped configuration without writing",
          "[functional][auth][https]") {
    const auto sandbox = CreateSandboxWorkspace("https-auth-dry-run");
    const auto home = sandbox.root / "home";
    const auto fakeGcm = sandbox.root / "git-credential-manager";
    std::filesystem::create_directories(home);
    CreateFakeGcm(fakeGcm);

    const auto result = RunKogWithEnv(
        {
            "auth",
            "https",
            "setup",
            "--hostname",
            "gitlab.example.com",
            "--username",
            "kano-user",
            "--gcm-path",
            fakeGcm.string(),
            "--dry-run",
        },
        sandbox.root,
        HttpsAuthEnvironment(home));
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("[dry-run] GCM helper:") != std::string::npos);
    REQUIRE(result.stdoutText.find("GitLab HTTPS host: gitlab.example.com auth-mode=pat") !=
            std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(home / "global.gitconfig"));

    RemoveSandboxWorkspace(sandbox);
}

#if !defined(_WIN32)
TEST_CASE("https auth install dry-run identifies the pinned release and checksum",
          "[functional][auth][https]") {
    const auto sandbox = CreateSandboxWorkspace("https-auth-install-dry-run");
    const auto home = sandbox.root / "home";
    std::filesystem::create_directories(home);

    const auto result = RunKogWithEnv(
        {
            "auth",
            "https",
            "setup",
            "--hostname",
            "gitlab.example.com",
            "--gcm-path",
            (sandbox.root / "missing-gcm").string(),
            "--install",
            "--dry-run",
        },
        sandbox.root,
        HttpsAuthEnvironment(home));
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find(
                "https://github.com/git-ecosystem/git-credential-manager/releases/download/"
                "v2.9.1/") != std::string::npos);
    REQUIRE(result.stdoutText.find("[dry-run] verify SHA-256:") != std::string::npos);
    REQUIRE(result.stdoutText.find(
                ".local/share/kog/git-credential-manager/2.9.1") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(home / "global.gitconfig"));

    RemoveSandboxWorkspace(sandbox);
}
#endif

TEST_CASE("https auth setup is confirm-gated idempotent and doctor-verifiable",
          "[functional][auth][https]") {
    const auto sandbox = CreateSandboxWorkspace("https-auth-setup");
    const auto home = sandbox.root / "home";
    const auto fakeGcm = sandbox.root / "git-credential-manager";
    std::filesystem::create_directories(home);
    CreateFakeGcm(fakeGcm);
    const auto env = HttpsAuthEnvironment(home);
    const std::vector<std::string> setupArgs{
        "auth",
        "https",
        "setup",
        "--hostname",
        "gitlab.example.com",
        "--username",
        "kano-user",
        "--gcm-path",
        fakeGcm.string(),
        "--confirm-global-write",
    };

    const auto first = RunKogWithEnv(setupArgs, sandbox.root, env);
    INFO(first.stdoutText);
    INFO(first.stderrText);
    REQUIRE(first.exitCode == 0);

    const auto second = RunKogWithEnv(setupArgs, sandbox.root, env);
    INFO(second.stdoutText);
    INFO(second.stderrText);
    REQUIRE(second.exitCode == 0);

    const auto config = ReadTextFile(home / "global.gitconfig");
    REQUIRE(config.find("helper = " + fakeGcm.string()) != std::string::npos);
    REQUIRE(config.find("provider = gitlab") != std::string::npos);
    REQUIRE(config.find("gitLabAuthModes = pat") != std::string::npos);
    REQUIRE(config.find("username = kano-user") != std::string::npos);

    const auto doctor = RunKogWithEnv(
        {
            "auth",
            "https",
            "doctor",
            "--hostname",
            "gitlab.example.com",
            "--gcm-path",
            fakeGcm.string(),
        },
        sandbox.root,
        env);
    INFO(doctor.stdoutText);
    INFO(doctor.stderrText);
    REQUIRE(doctor.exitCode == 0);
    REQUIRE(doctor.stdoutText.find("credential_helper=configured") != std::string::npos);
    REQUIRE(doctor.stdoutText.find("provider=gitlab") != std::string::npos);
    REQUIRE(doctor.stdoutText.find("auth_mode=pat") != std::string::npos);
    REQUIRE(doctor.stdoutText.find("status=ready") != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
