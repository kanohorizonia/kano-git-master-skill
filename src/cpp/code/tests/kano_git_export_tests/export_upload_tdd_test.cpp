#include <catch2/catch_test_macros.hpp>

#include "export_helpers.hpp"
#include "shell_executor.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace kano::git::commands;
using namespace kano::git::shell;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("kog_export_upload_tdd_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct RecordedCommand {
    std::string command;
    std::vector<std::string> args;
};

struct ScopedCurrentPath {
    std::filesystem::path previous;

    explicit ScopedCurrentPath(const std::filesystem::path& InPath)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(InPath);
    }

    ~ScopedCurrentPath() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }
};

auto SetEnvForTest(const std::string& InName, const std::optional<std::string>& InValue) -> void {
#if defined(_WIN32)
    _putenv_s(InName.c_str(), InValue.value_or("").c_str());
#else
    if (InValue.has_value()) {
        setenv(InName.c_str(), InValue->c_str(), 1);
    } else {
        unsetenv(InName.c_str());
    }
#endif
}

auto ReadEnvForTest(const std::string& InName) -> std::optional<std::string> {
    const char* value = std::getenv(InName.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

struct ScopedHomeEnv {
    std::optional<std::string> previousUserProfile;
    std::optional<std::string> previousHome;

    explicit ScopedHomeEnv(const std::filesystem::path& InHome)
        : previousUserProfile(ReadEnvForTest("USERPROFILE")),
          previousHome(ReadEnvForTest("HOME")) {
        SetEnvForTest("USERPROFILE", InHome.string());
        SetEnvForTest("HOME", InHome.string());
    }

    ~ScopedHomeEnv() {
        SetEnvForTest("USERPROFILE", previousUserProfile);
        SetEnvForTest("HOME", previousHome);
    }
};

struct CaptureStdout {
    std::ostringstream buffer;
    std::streambuf* previous = nullptr;

    CaptureStdout()
        : previous(std::cout.rdbuf(buffer.rdbuf())) {}

    ~CaptureStdout() {
        std::cout.rdbuf(previous);
    }

    auto str() const -> std::string {
        return buffer.str();
    }
};

auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto ReadTextFile(const std::filesystem::path& InPath) -> std::string {
    std::ifstream in(InPath, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto MakeRequest(const std::filesystem::path& InArchive,
                 const std::filesystem::path& InManifest,
                 ExportUploadConfig InConfig) -> ExportUploadRequest {
    ExportUploadRequest request;
    request.archivePath = InArchive;
    request.manifestPath = InManifest;
    request.archiveName = InArchive.filename().string();
    request.config = std::move(InConfig);
    return request;
}

auto MakeLocalSyncConfig(const std::filesystem::path& InTarget) -> ExportUploadConfig {
    ExportUploadConfig config;
    config.target = "local-sync-folder";
    config.localSyncFolder = InTarget;
    return config;
}

auto MakeRcloneConfig() -> ExportUploadConfig {
    ExportUploadConfig config;
    config.target = "rclone";
    config.rcloneRemote = "kog-drive";
    config.rcloneDestination = "exports/kog";
    return config;
}

auto NoopExecutor() -> ShellExecutor {
    return [](const std::string& /*InCommand*/,
              const std::vector<std::string>& /*InArgs*/,
              ExecMode /*InMode*/,
              std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        result.exitCode = 0;
        return result;
    };
}

auto ContainsArg(const std::vector<std::string>& InArgs, const std::string& InNeedle) -> bool {
    for (const auto& arg : InArgs) {
        if (arg == InNeedle) {
            return true;
        }
    }
    return false;
}

auto CommandWasInvoked(const std::vector<RecordedCommand>& InCalls,
                       const std::string& InCommand,
                       const std::string& InFirstArg) -> bool {
    for (const auto& call : InCalls) {
        if (call.command == InCommand && !call.args.empty() && call.args.front() == InFirstArg) {
            return true;
        }
    }
    return false;
}

auto CountRcloneCopyToContaining(const std::vector<RecordedCommand>& InCalls,
                                 const std::string& InNeedle) -> int {
    int count = 0;
    for (const auto& call : InCalls) {
        if (call.command != "rclone" || call.args.empty() || call.args.front() != "copyto") {
            continue;
        }
        for (const auto& arg : call.args) {
            if (arg.find(InNeedle) != std::string::npos) {
                ++count;
                break;
            }
        }
    }
    return count;
}

auto MakeSuccessfulRcloneExecutor(std::vector<RecordedCommand>& InCalls,
                                  std::optional<std::string> InDriveId,
                                  std::optional<std::string> InPublicLink = std::nullopt) -> ShellExecutor {
    return [&InCalls, InDriveId, InPublicLink](const std::string& InCommand,
                                               const std::vector<std::string>& InArgs,
                                               ExecMode /*InMode*/,
                                               std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        InCalls.push_back({InCommand, InArgs});

        ExecResult result;
        result.exitCode = 0;
        if (InCommand != "rclone") {
            result.exitCode = 127;
            result.stderrStr = "unexpected command";
            return result;
        }

        if (!InArgs.empty() && InArgs.front() == "listremotes") {
            result.stdoutStr = "kog-drive:\n";
            return result;
        }
        if (!InArgs.empty() && (InArgs.front() == "copy" || InArgs.front() == "copyto")) {
            return result;
        }
        if (!InArgs.empty() && InArgs.front() == "lsjson") {
            REQUIRE(ContainsArg(InArgs, "--stat"));
            REQUIRE(ContainsArg(InArgs, "-M"));
            result.stdoutStr = InDriveId.has_value() ? ("{ \"ID\": \"" + *InDriveId + "\" }\n") : "{}\n";
            return result;
        }
        if (!InArgs.empty() && InArgs.front() == "link") {
            if (InPublicLink.has_value()) {
                result.stdoutStr = *InPublicLink;
            } else {
                result.exitCode = 23;
                result.stderrStr = "rclone link mutates Drive permissions and should be explicit";
            }
            return result;
        }

        return result;
    };
}

auto MakeFailingRcloneExecutor(std::vector<RecordedCommand>& InCalls,
                               const std::string& InFailOperation) -> ShellExecutor {
    return [&InCalls, InFailOperation](const std::string& InCommand,
                                       const std::vector<std::string>& InArgs,
                                       ExecMode /*InMode*/,
                                       std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        InCalls.push_back({InCommand, InArgs});

        ExecResult result;
        result.exitCode = 0;
        if (InCommand != "rclone") {
            result.exitCode = 127;
            result.stderrStr = "unexpected command";
            return result;
        }

        if (!InArgs.empty() && InArgs.front() == "copyto") {
            const bool isSidecar = InArgs.size() > 1 && InArgs[1].find(".sha256") != std::string::npos;
            const bool isUploadManifest = InArgs.size() > 1 && InArgs[1].find(".upload-manifest.json") != std::string::npos;
            if ((InFailOperation == "sidecar" && isSidecar) ||
                (InFailOperation == "upload-manifest" && isUploadManifest)) {
                result.exitCode = 1;
                result.stderrStr = InFailOperation + " failed with token=secret-value";
            }
            return result;
        }
        if (!InArgs.empty() && InArgs.front() == "lsjson") {
            result.stdoutStr = "{ \"ID\": \"drive-file-id-123\" }\n";
            return result;
        }
        if (!InArgs.empty() && InArgs.front() == "link") {
            result.exitCode = 1;
            result.stderrStr = "link failed with password=hunter2";
            return result;
        }
        return result;
    };
}

} // anonymous namespace

TEST_CASE("local-sync-folder upload copies archive manifest and sha sidecar",
          "[tdd][unit][feature:kog-export-upload][local-sync-folder]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    const auto target = tmp.path / "sync-target";
    std::filesystem::create_directories(target);
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");
    auto config = MakeLocalSyncConfig(target);
    config.layout = "ChatGPT_Export/kog";
    config.copyManifest = true;
    config.copySha256 = true;

    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, config), NoopExecutor());

    REQUIRE(result.success);
    const auto uploadDir = target / "ChatGPT_Export" / "kog";
    REQUIRE(result.syncFolderPath == target);
    REQUIRE(result.localTargetPath == uploadDir);
    REQUIRE(std::filesystem::exists(uploadDir / archive.filename()));
    REQUIRE(std::filesystem::exists(uploadDir / manifest.filename()));
    REQUIRE(std::filesystem::exists(result.sha256SidecarPath));
    REQUIRE(ReadTextFile(uploadDir / archive.filename()) == ReadTextFile(archive));
    REQUIRE(std::filesystem::exists(result.uploadManifestPath));
    REQUIRE(result.output.find("Cloud sync is handled externally") != std::string::npos);
    REQUIRE_FALSE(result.fileId.has_value());
    REQUIRE_FALSE(result.webUrl.has_value());
    const std::string uploadManifest = ReadTextFile(result.uploadManifestPath);
    REQUIRE(uploadManifest.find("\"kind\": \"kog-export-upload\"") != std::string::npos);
    REQUIRE(uploadManifest.find("\"sourceArchive\"") != std::string::npos);
    REQUIRE(uploadManifest.find("\"sourceSha256\"") != std::string::npos);
    REQUIRE(uploadManifest.find("\"localTargetPath\"") != std::string::npos);
    REQUIRE(uploadManifest.find("\"syncFolderPath\"") != std::string::npos);
    REQUIRE(uploadManifest.find("ChatGPT_Export/kog") != std::string::npos);
}

TEST_CASE("local-sync-folder copies only archive by default while still verifying sha",
          "[tdd][unit][feature:kog-export-upload][local-sync-folder]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    const auto target = tmp.path / "sync-target";
    std::filesystem::create_directories(target);
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, MakeLocalSyncConfig(target)), NoopExecutor());

    REQUIRE(result.success);
    REQUIRE(std::filesystem::exists(target / archive.filename()));
    REQUIRE_FALSE(std::filesystem::exists(target / manifest.filename()));
    REQUIRE(result.sha256SidecarPath.empty());
    const std::string uploadManifest = ReadTextFile(result.uploadManifestPath);
    REQUIRE(uploadManifest.find("\"copyManifest\": false") != std::string::npos);
    REQUIRE(uploadManifest.find("\"copySha256\": false") != std::string::npos);
    REQUIRE(uploadManifest.find("\"sourceSha256\"") != std::string::npos);
}

TEST_CASE("local-sync-folder missing path is diagnosed and upload leaves source untouched",
          "[tdd][unit][feature:kog-export-upload][local-sync-folder]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    const auto missingTarget = tmp.path / "missing" / "sync-target";
    WriteTextFile(archive, "original archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    const ExportUploadConfig config = MakeLocalSyncConfig(missingTarget);
    const auto doctor = DoctorExportUploadWithExecutor(config, NoopExecutor());
    REQUIRE_FALSE(doctor.ok);
    REQUIRE(doctor.status == "MISSING_PATH");
    REQUIRE(doctor.guidance.find("local-sync-folder") != std::string::npos);

    const auto result = UploadExportArtifactsWithExecutor(MakeRequest(archive, manifest, config), NoopExecutor());
    REQUIRE_FALSE(result.success);
    REQUIRE(result.errorMessage.find("local-sync-folder") != std::string::npos);
    REQUIRE(ReadTextFile(archive) == "original archive bytes\n");
}

TEST_CASE("local-sync-folder rejects unsafe layout before copying",
          "[tdd][unit][feature:kog-export-upload][local-sync-folder][security]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    const auto target = tmp.path / "sync-target";
    std::filesystem::create_directories(target);
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    auto config = MakeLocalSyncConfig(target);
    config.layout = "../escape";

    const auto doctor = DoctorExportUploadWithExecutor(config, NoopExecutor());
    REQUIRE_FALSE(doctor.ok);
    REQUIRE(doctor.status == "INVALID_CONFIG");

    const auto result = UploadExportArtifactsWithExecutor(MakeRequest(archive, manifest, config), NoopExecutor());
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(std::filesystem::exists(tmp.path / "escape" / archive.filename()));
}

TEST_CASE("rclone doctor reports missing binary as third-party setup issue",
          "[tdd][unit][feature:kog-export-upload][rclone][doctor]") {
    const auto exec = [](const std::string& InCommand,
                         const std::vector<std::string>& /*InArgs*/,
                         ExecMode /*InMode*/,
                         std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        REQUIRE(InCommand == "rclone");
        result.exitCode = 127;
        result.stderrStr = "rclone: command not found";
        return result;
    };

    const auto doctor = DoctorExportUploadWithExecutor(MakeRcloneConfig(), exec);
    REQUIRE_FALSE(doctor.ok);
    REQUIRE(doctor.status == "RCLONE_NOT_FOUND");
    REQUIRE(doctor.thirdParty);
    REQUIRE(doctor.backendLabel.find("third-party") != std::string::npos);
    REQUIRE(doctor.guidance.find("install") != std::string::npos);
    REQUIRE(doctor.guidance.find("rclone") != std::string::npos);
}

TEST_CASE("rclone doctor rejects unsafe remote config before invoking rclone",
          "[tdd][unit][feature:kog-export-upload][rclone][doctor][security]") {
    auto config = MakeRcloneConfig();
    config.rcloneRemote = ":drive,token=secret";

    std::vector<RecordedCommand> calls;
    const auto exec = [&calls](const std::string& InCommand,
                               const std::vector<std::string>& InArgs,
                               ExecMode /*InMode*/,
                               std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        calls.push_back({InCommand, InArgs});
        ExecResult result;
        result.exitCode = 0;
        return result;
    };

    const auto doctor = DoctorExportUploadWithExecutor(config, exec);
    REQUIRE_FALSE(doctor.ok);
    REQUIRE(doctor.status == "INVALID_CONFIG");
    REQUIRE(doctor.guidance.find("inline credentials") != std::string::npos);
    REQUIRE(calls.empty());
}

TEST_CASE("rclone doctor reports missing configured remote when listremotes is empty",
          "[tdd][unit][feature:kog-export-upload][rclone][doctor]") {
    const auto exec = [](const std::string& InCommand,
                         const std::vector<std::string>& InArgs,
                         ExecMode /*InMode*/,
                         std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        ExecResult result;
        REQUIRE(InCommand == "rclone");
        result.exitCode = 0;
        if (!InArgs.empty() && InArgs.front() == "listremotes") {
            result.stdoutStr = "";
        }
        return result;
    };

    const auto doctor = DoctorExportUploadWithExecutor(MakeRcloneConfig(), exec);
    REQUIRE_FALSE(doctor.ok);
    REQUIRE(doctor.status == "RCLONE_REMOTE_MISSING");
    REQUIRE(doctor.guidance.find("rclone config") != std::string::npos);
}

TEST_CASE("rclone private upload records Drive file ID URL without public link mutation",
          "[tdd][unit][feature:kog-export-upload][rclone]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    std::vector<RecordedCommand> calls;
    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, MakeRcloneConfig()),
        MakeSuccessfulRcloneExecutor(calls, "drive-file-id-123"));

    REQUIRE(result.success);
    REQUIRE(result.fileId == std::optional<std::string>("drive-file-id-123"));
    REQUIRE(result.webUrl == std::optional<std::string>("https://drive.google.com/file/d/drive-file-id-123/view"));
    REQUIRE((result.visibility == "private" || result.visibility == "preserve"));
    REQUIRE_FALSE(result.permissionChanged);
    REQUIRE(std::filesystem::exists(result.uploadManifestPath));
    const std::string uploadManifest = ReadTextFile(result.uploadManifestPath);
    REQUIRE(uploadManifest.find("\"fileId\"") != std::string::npos);
    REQUIRE(uploadManifest.find("drive-file-id-123") != std::string::npos);
    REQUIRE_FALSE(CommandWasInvoked(calls, "rclone", "link"));
}

TEST_CASE("rclone upload surfaces URL_UNAVAILABLE when Drive ID is unavailable",
          "[tdd][unit][feature:kog-export-upload][rclone]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    std::vector<RecordedCommand> calls;
    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, MakeRcloneConfig()),
        MakeSuccessfulRcloneExecutor(calls, std::nullopt));

    REQUIRE(result.success);
    REQUIRE_FALSE(result.remotePath.empty());
    REQUIRE_FALSE(result.webUrl.has_value());
    REQUIRE(result.urlStatus == "URL_UNAVAILABLE");
    REQUIRE_FALSE(CommandWasInvoked(calls, "rclone", "link"));
}

TEST_CASE("rclone public link is blocked by default and allowed only by explicit flag",
          "[tdd][unit][feature:kog-export-upload][rclone][public-link]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    SECTION("default private upload does not call rclone link") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.publicLink = false;
        config.yes = false;

        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeSuccessfulRcloneExecutor(calls, "drive-file-id-private"));

        REQUIRE(result.success);
        REQUIRE_FALSE(CommandWasInvoked(calls, "rclone", "link"));
        REQUIRE_FALSE(result.permissionChanged);
    }

    SECTION("public link without yes is kept private and does not call rclone link") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.publicLink = true;
        config.yes = false;

        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeSuccessfulRcloneExecutor(calls, "drive-file-id-private"));

        REQUIRE(result.success);
        REQUIRE_FALSE(CommandWasInvoked(calls, "rclone", "link"));
        REQUIRE_FALSE(result.permissionChanged);
        REQUIRE(result.visibility == "private");
        REQUIRE(result.output.find("without --yes") != std::string::npos);
    }

    SECTION("explicit --public-link --yes may invoke permission-mutating link") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.publicLink = true;
        config.yes = true;

        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeSuccessfulRcloneExecutor(calls, "drive-file-id-public", "https://example.invalid/public-link"));

        REQUIRE(result.success);
        REQUIRE(CommandWasInvoked(calls, "rclone", "link"));
        REQUIRE(result.webUrl == std::optional<std::string>("https://example.invalid/public-link"));
        REQUIRE(result.output.find("public link") != std::string::npos);
        REQUIRE(result.visibility == "public-link");
        REQUIRE(result.permissionChanged);
    }
}

TEST_CASE("rclone upload rejects unsafe remote config before invoking rclone",
          "[tdd][unit][feature:kog-export-upload][rclone][security]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    auto config = MakeRcloneConfig();
    config.rcloneRemote = ":drive,token=secret";

    std::vector<RecordedCommand> calls;
    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, config),
        MakeSuccessfulRcloneExecutor(calls, "drive-file-id-123"));

    REQUIRE_FALSE(result.success);
    REQUIRE(result.errorMessage.find("inline credentials") != std::string::npos);
    REQUIRE(calls.empty());
}

TEST_CASE("rclone upload fails when required remote metadata copies fail",
          "[tdd][unit][feature:kog-export-upload][rclone]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    SECTION("sha256 sidecar copy failure fails upload") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.copySha256 = true;
        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeFailingRcloneExecutor(calls, "sidecar"));

        REQUIRE_FALSE(result.success);
        REQUIRE(result.errorMessage.find("sha256 sidecar") != std::string::npos);
        REQUIRE(result.errorMessage.find("secret-value") == std::string::npos);
    }

    SECTION("upload manifest copy failure fails upload") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.copySha256 = true;
        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeFailingRcloneExecutor(calls, "upload-manifest"));

        REQUIRE_FALSE(result.success);
        REQUIRE(result.errorMessage.find("upload manifest") != std::string::npos);
        REQUIRE(result.errorMessage.find("secret-value") == std::string::npos);
    }
}

TEST_CASE("rclone upload copies original export manifest only when enabled",
          "[tdd][unit][feature:kog-export-upload][rclone]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001.export-manifest.json";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "{\"archiveFile\":\"skill_rev001.tar\"}\n");

    SECTION("disabled by default") {
        std::vector<RecordedCommand> calls;
        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, MakeRcloneConfig()),
            MakeSuccessfulRcloneExecutor(calls, "drive-file-id-123"));

        REQUIRE(result.success);
        REQUIRE(CountRcloneCopyToContaining(calls, ".export-manifest.json") == 0);
        REQUIRE(CountRcloneCopyToContaining(calls, ".sha256") == 0);
    }

    SECTION("enabled copies manifest and sha sidecar") {
        std::vector<RecordedCommand> calls;
        auto config = MakeRcloneConfig();
        config.copyManifest = true;
        config.copySha256 = true;
        config.layout = "ChatGPT_Export/kog";

        const auto result = UploadExportArtifactsWithExecutor(
            MakeRequest(archive, manifest, config),
            MakeSuccessfulRcloneExecutor(calls, "drive-file-id-123"));

        REQUIRE(result.success);
        REQUIRE(CountRcloneCopyToContaining(calls, ".export-manifest.json") == 1);
        REQUIRE(CountRcloneCopyToContaining(calls, ".sha256") == 1);
        REQUIRE(result.remotePath.find("exports/kog/ChatGPT_Export/kog/skill_rev001.tar") != std::string::npos);
        const std::string uploadManifest = ReadTextFile(result.uploadManifestPath);
        REQUIRE(uploadManifest.find("\"remoteArchiveName\": \"skill_rev001.tar\"") != std::string::npos);
        REQUIRE(uploadManifest.find("\"backend\": \"rclone\"") != std::string::npos);
    }
}

TEST_CASE("rclone public link failure is not reported as public success",
          "[tdd][unit][feature:kog-export-upload][rclone][public-link]") {
    TempDir tmp;
    const auto archive = tmp.path / "out" / "skill_rev001.tar";
    const auto manifest = tmp.path / "out" / "skill_rev001_manifest.txt";
    WriteTextFile(archive, "archive bytes\n");
    WriteTextFile(manifest, "manifest bytes\n");

    auto config = MakeRcloneConfig();
    config.publicLink = true;
    config.yes = true;

    std::vector<RecordedCommand> calls;
    const auto result = UploadExportArtifactsWithExecutor(
        MakeRequest(archive, manifest, config),
        MakeFailingRcloneExecutor(calls, "link"));

    REQUIRE_FALSE(result.success);
    REQUIRE(CommandWasInvoked(calls, "rclone", "link"));
    REQUIRE_FALSE(result.permissionChanged);
    REQUIRE(result.visibility == "private");
    REQUIRE(result.errorMessage.find("public link") != std::string::npos);
    REQUIRE(result.errorMessage.find("hunter2") == std::string::npos);
}

TEST_CASE("Google Drive API upload doctor is guidance only and does not start OAuth",
          "[tdd][unit][feature:kog-export-upload][gdrive-api][doctor]") {
    std::vector<RecordedCommand> calls;
    const auto exec = [&calls](const std::string& InCommand,
                               const std::vector<std::string>& InArgs,
                               ExecMode /*InMode*/,
                               std::optional<std::filesystem::path> /*InWorkingDir*/) -> ExecResult {
        calls.push_back({InCommand, InArgs});
        ExecResult result;
        result.exitCode = 0;
        return result;
    };

    ExportUploadConfig config;
    config.target = "gdrive-api";

    const auto doctor = DoctorExportUploadWithExecutor(config, exec);
    REQUIRE_FALSE(doctor.ok);
    REQUIRE((doctor.status == "NOT_CONFIGURED" || doctor.status == "FUTURE_BACKEND"));
    REQUIRE(doctor.guidance.find("Google Drive API") != std::string::npos);
    REQUIRE(calls.empty());
}

TEST_CASE("export upload config precedence is CLI over repo over user with safe defaults",
          "[tdd][unit][feature:kog-export-upload][config]") {
    ExportUploadConfigLayer user;
    user.target = "rclone";
    user.rcloneRemote = "user-drive";
    user.rcloneDestination = "user/exports";
    user.layout = "user-layout";
    user.copyManifest = true;
    user.copySha256 = true;
    user.returnUrl = false;
    user.linkMode = "public-link";
    user.publicLink = true;

    ExportUploadConfigLayer repo;
    repo.target = "local-sync-folder";
    repo.localSyncFolder = std::filesystem::path("repo-sync");
    repo.layout = "repo-layout";
    repo.copyManifest = false;
    repo.copySha256 = true;
    repo.returnUrl = true;
    repo.publicLink = false;

    ExportUploadConfigLayer cli;
    cli.target = "rclone";
    cli.rcloneRemote = "cli-drive";
    cli.rcloneDestination = "cli/exports";
    cli.layout = "cli-layout";
    cli.copyManifest = true;

    const auto cliEffective = ResolveExportUploadConfig(user, repo, cli);
    REQUIRE(cliEffective.target == "rclone");
    REQUIRE(cliEffective.rcloneRemote == "cli-drive");
    REQUIRE(cliEffective.rcloneDestination == "cli/exports");
    REQUIRE(cliEffective.layout == "cli-layout");
    REQUIRE(cliEffective.copyManifest);
    REQUIRE(cliEffective.copySha256);
    REQUIRE(cliEffective.returnUrl);
    REQUIRE_FALSE(cliEffective.publicLink);
    REQUIRE(cliEffective.linkMode == "private");

    const auto repoEffective = ResolveExportUploadConfig(user, repo, ExportUploadConfigLayer{});
    REQUIRE(repoEffective.target == "local-sync-folder");
    REQUIRE(repoEffective.localSyncFolder == std::filesystem::path("repo-sync"));
    REQUIRE(repoEffective.layout == "repo-layout");
    REQUIRE_FALSE(repoEffective.copyManifest);
    REQUIRE(repoEffective.copySha256);
    REQUIRE(repoEffective.returnUrl);
    REQUIRE_FALSE(repoEffective.publicLink);
    REQUIRE(repoEffective.linkMode == "private");

    const auto defaults = ResolveExportUploadConfig(ExportUploadConfigLayer{}, ExportUploadConfigLayer{}, ExportUploadConfigLayer{});
    REQUIRE(defaults.target.empty());
    REQUIRE_FALSE(defaults.copyManifest);
    REQUIRE_FALSE(defaults.copySha256);
    REQUIRE(defaults.returnUrl);
    REQUIRE(defaults.linkMode == "private");
    REQUIRE_FALSE(defaults.publicLink);
    REQUIRE_FALSE(defaults.yes);
}

TEST_CASE("export upload config cannot enable permission mutation without CLI flags",
          "[tdd][unit][feature:kog-export-upload][config][public-link]") {
    ExportUploadConfigLayer user;
    user.target = "rclone";
    user.rcloneRemote = "user-drive";
    user.publicLink = true;
    user.yes = true;

    ExportUploadConfigLayer repo;
    repo.target = "rclone";
    repo.rcloneRemote = "repo-drive";
    repo.publicLink = true;
    repo.yes = true;

    const auto configOnly = ResolveExportUploadConfig(user, repo, ExportUploadConfigLayer{});
    REQUIRE(configOnly.target == "rclone");
    REQUIRE(configOnly.rcloneRemote == "repo-drive");
    REQUIRE_FALSE(configOnly.publicLink);
    REQUIRE_FALSE(configOnly.yes);

    ExportUploadConfigLayer publicLinkOnly;
    publicLinkOnly.publicLink = true;
    const auto publicLinkWithoutYes = ResolveExportUploadConfig(user, repo, publicLinkOnly);
    REQUIRE(publicLinkWithoutYes.publicLink);
    REQUIRE_FALSE(publicLinkWithoutYes.yes);

    ExportUploadConfigLayer explicitCli;
    explicitCli.publicLink = true;
    explicitCli.yes = true;
    const auto explicitPublicLink = ResolveExportUploadConfig(user, repo, explicitCli);
    REQUIRE(explicitPublicLink.publicLink);
    REQUIRE(explicitPublicLink.yes);
}

TEST_CASE("export upload CLI config uses repo kog_config default target over user default",
          "[tdd][unit][feature:kog-export-upload][config][cli]") {
    TempDir tmp;
    const auto userHome = tmp.path / "user-home";
    const auto project = tmp.path / "project";
    const auto userSync = tmp.path / "user-sync";
    const auto repoSync = tmp.path / "repo-sync";
    std::filesystem::create_directories(userSync);
    std::filesystem::create_directories(repoSync);
    std::filesystem::create_directories(project / ".kano");
    std::filesystem::create_directories(userHome / ".kano");

    WriteTextFile(userHome / ".kano" / "kog_config.toml",
                  "[export.upload]\n"
                  "default_target = \"drive_sync\"\n"
                  "[export.upload.targets.drive_sync]\n"
                  "type = \"local-sync-folder\"\n"
                  "enabled = true\n"
                  "path = \"" + userSync.generic_string() + "\"\n");
    WriteTextFile(project / ".kano" / "kog_config.toml",
                  "[export.upload]\n"
                  "default_target = \"drive_sync\"\n"
                  "[export.upload.targets.drive_sync]\n"
                  "type = \"local-sync-folder\"\n"
                  "enabled = true\n"
                  "layout = \"ChatGPT_Export/kog\"\n"
                  "copy_manifest = true\n"
                  "copy_sha256 = true\n"
                  "path = \"" + repoSync.generic_string() + "\"\n");

    ScopedHomeEnv home(userHome);
    ScopedCurrentPath cwd(project);
    CaptureStdout stdoutCapture;

    CLI::App app{"kog test"};
    RegisterExport(app);
    std::vector<std::string> args{"doctor", "upload", "export"};
    app.parse(args);

    const std::string out = stdoutCapture.str();
    REQUIRE(out.find(repoSync.generic_string()) != std::string::npos);
    REQUIRE(out.find("ChatGPT_Export/kog") != std::string::npos);
    REQUIRE(out.find("copy_manifest: true") != std::string::npos);
    REQUIRE(out.find("copy_sha256: true") != std::string::npos);
    REQUIRE(out.find(userSync.generic_string()) == std::string::npos);
}

TEST_CASE("export upload CLI target overrides configured default target section",
          "[tdd][unit][feature:kog-export-upload][config][cli]") {
    TempDir tmp;
    const auto userHome = tmp.path / "user-home";
    const auto project = tmp.path / "project";
    const auto defaultSync = tmp.path / "default-sync";
    const auto cliSync = tmp.path / "cli-sync";
    std::filesystem::create_directories(defaultSync);
    std::filesystem::create_directories(cliSync);
    std::filesystem::create_directories(project / ".kano");
    std::filesystem::create_directories(userHome / ".kano");

    WriteTextFile(userHome / ".kano" / "kog_config.toml",
                  "[export.upload]\n"
                  "default_target = \"drive_sync\"\n"
                  "[export.upload.targets.drive_sync]\n"
                  "type = \"local-sync-folder\"\n"
                  "path = \"" + defaultSync.generic_string() + "\"\n");
    WriteTextFile(project / ".kano" / "kog_config.toml",
                  "[export.upload]\n"
                  "default_target = \"drive_sync\"\n"
                  "[export.upload.targets.drive_sync]\n"
                  "type = \"local-sync-folder\"\n"
                  "path = \"" + defaultSync.generic_string() + "\"\n"
                  "[export.upload.targets.cli_sync]\n"
                  "type = \"local-sync-folder\"\n"
                  "path = \"" + cliSync.generic_string() + "\"\n");

    ScopedHomeEnv home(userHome);
    ScopedCurrentPath cwd(project);
    CaptureStdout stdoutCapture;

    CLI::App app{"kog test"};
    RegisterExport(app);
    std::vector<std::string> args{"cli_sync", "--target", "doctor", "upload", "export"};
    app.parse(args);

    const std::string out = stdoutCapture.str();
    REQUIRE(out.find(cliSync.generic_string()) != std::string::npos);
    REQUIRE(out.find(defaultSync.generic_string()) == std::string::npos);
}

TEST_CASE("export upload --last rejects manifest archive paths outside .kano tmp",
          "[tdd][unit][feature:kog-export-upload][security][cli]") {
    TempDir tmp;
    const auto project = tmp.path / "project";
    const auto tmpRoot = project / ".kano" / "tmp";
    const auto externalArchive = tmp.path / "outside" / "stolen.tar";
    std::filesystem::create_directories(tmpRoot);
    WriteTextFile(externalArchive, "outside archive bytes\n");
    WriteTextFile(tmpRoot / "tampered.export-manifest.json",
                  "{\n"
                  "  \"archiveFile\": \"" + externalArchive.generic_string() + "\"\n"
                  "}\n");

    ScopedCurrentPath cwd(project);
    CLI::App app{"kog test"};
    RegisterExport(app);
    std::vector<std::string> args{"--last", "upload", "export"};

    REQUIRE_THROWS_AS(app.parse(args), CLI::RuntimeError);
}

TEST_CASE("export upload diagnostics redact token and password-like values",
          "[tdd][unit][feature:kog-export-upload][redaction]") {
    const std::string unsafe =
        "rclone copy --drive-token ya29.secret-token --password hunter2 "
        "--client-secret cli-secret --passwd cli-passwd "
        "url=https://user:pass@example.invalid/path api_token=plain-secret client_secret=config-secret passwd=config-passwd";

    const std::string safe = RedactExportUploadText(unsafe);
    REQUIRE(safe.find("ya29.secret-token") == std::string::npos);
    REQUIRE(safe.find("hunter2") == std::string::npos);
    REQUIRE(safe.find("plain-secret") == std::string::npos);
    REQUIRE(safe.find("cli-secret") == std::string::npos);
    REQUIRE(safe.find("cli-passwd") == std::string::npos);
    REQUIRE(safe.find("config-secret") == std::string::npos);
    REQUIRE(safe.find("config-passwd") == std::string::npos);
    REQUIRE(safe.find("user:pass@example") == std::string::npos);
    REQUIRE(safe.find("<redacted>") != std::string::npos);
}
