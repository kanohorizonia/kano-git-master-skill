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

auto CountOccurrences(const std::string& InText, const std::string& InNeedle) -> std::size_t {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = InText.find(InNeedle, offset)) != std::string::npos) {
        ++count;
        offset += InNeedle.size();
    }
    return count;
}

auto CreateFakeCloudflared(const std::filesystem::path& InPath) -> void {
    std::ofstream output(InPath, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
    output << "@echo off\r\nexit /b 0\r\n";
#else
    output << "#!/usr/bin/env sh\nexit 0\n";
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

auto CloudflareSshEnvironment(const std::filesystem::path& InHome)
    -> std::vector<std::pair<std::string, std::string>> {
    return {
        {"HOME", InHome.string()},
        {"USERPROFILE", InHome.string()},
        {"KOG_TEST_MODE", "1"},
    };
}

} // namespace

TEST_CASE("cloudflare ssh setup dry-run previews configuration without writing",
          "[functional][auth][cloudflare-ssh]") {
    const auto sandbox = CreateSandboxWorkspace("cloudflare-ssh-dry-run");
    const auto home = sandbox.root / "home";
    const auto fakeCloudflared = sandbox.root / "cloudflared";
    std::filesystem::create_directories(home);
    CreateFakeCloudflared(fakeCloudflared);

    const auto result = RunKogWithEnv(
        {
            "auth",
            "cloudflare-ssh",
            "setup",
            "--hostname",
            "gitlab-ssh.example.com",
            "--cloudflared-path",
            fakeCloudflared.string(),
            "--dry-run",
        },
        sandbox.root,
        CloudflareSshEnvironment(home));
    INFO(result.stdoutText);
    INFO(result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("[dry-run] SSH include:") != std::string::npos);
    REQUIRE(result.stdoutText.find("Host gitlab-ssh.example.com") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(home / ".ssh" / "config"));
    REQUIRE_FALSE(std::filesystem::exists(home / ".ssh" / "config.d" / "kog-cloudflare-access.conf"));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("cloudflare ssh setup is confirm-gated idempotent and doctor-verifiable",
          "[functional][auth][cloudflare-ssh]") {
    const auto sandbox = CreateSandboxWorkspace("cloudflare-ssh-setup");
    const auto home = sandbox.root / "home";
    const auto fakeCloudflared = sandbox.root / "cloudflared";
    std::filesystem::create_directories(home);
    CreateFakeCloudflared(fakeCloudflared);
    std::filesystem::create_directories(home / ".ssh");
    {
        std::ofstream existingConfig(home / ".ssh" / "config", std::ios::binary | std::ios::trunc);
        existingConfig << "Host legacy.example.com\n"
                          "  User legacy-user\n";
        existingConfig.close();
        REQUIRE(existingConfig.good());
    }
    const auto env = CloudflareSshEnvironment(home);
    const std::vector<std::string> setupArgs{
        "auth",
        "cloudflare-ssh",
        "setup",
        "--hostname",
        "gitlab-ssh.example.com",
        "--user",
        "git",
        "--cloudflared-path",
        fakeCloudflared.string(),
        "--confirm-host-write",
    };

    const auto first = RunKogWithEnv(setupArgs, sandbox.root, env);
    INFO(first.stdoutText);
    INFO(first.stderrText);
    REQUIRE(first.exitCode == 0);

    const auto second = RunKogWithEnv(setupArgs, sandbox.root, env);
    INFO(second.stdoutText);
    INFO(second.stderrText);
    REQUIRE(second.exitCode == 0);

    const auto mainConfig = ReadTextFile(home / ".ssh" / "config");
    const auto backupConfig = ReadTextFile(home / ".ssh" / "config.kog.bak");
    const auto fragment = ReadTextFile(home / ".ssh" / "config.d" / "kog-cloudflare-access.conf");
    REQUIRE(CountOccurrences(mainConfig, "Include ~/.ssh/config.d/*") == 1);
    REQUIRE(mainConfig.rfind("# BEGIN KOG CLOUDFLARE ACCESS INCLUDE", 0) == 0);
    REQUIRE(mainConfig.find("Host legacy.example.com") != std::string::npos);
    REQUIRE(backupConfig.find("Host legacy.example.com") != std::string::npos);
    REQUIRE(CountOccurrences(fragment, "# BEGIN KOG CLOUDFLARE ACCESS gitlab-ssh.example.com") == 1);
    REQUIRE(fragment.find("User git") != std::string::npos);
    REQUIRE(fragment.find("access ssh --hostname %h") != std::string::npos);

    const auto doctor = RunKogWithEnv(
        {
            "auth",
            "cloudflare-ssh",
            "doctor",
            "--hostname",
            "gitlab-ssh.example.com",
            "--cloudflared-path",
            fakeCloudflared.string(),
        },
        sandbox.root,
        env);
    INFO(doctor.stdoutText);
    INFO(doctor.stderrText);
    REQUIRE(doctor.exitCode == 0);
    REQUIRE(doctor.stdoutText.find("status=ready") != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

} // namespace kano::git::tests::functional
