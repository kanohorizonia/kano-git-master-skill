#pragma once

// export_helpers.hpp — Pure helper function declarations for kog export command.
// These functions are implemented in export_cmd.cpp (anonymous namespace) and
// exposed here for property-based and unit testing.

#include "discovery.hpp"
#include "shell_executor.hpp"

#include <CLI/CLI.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kano::git::commands {

// ---------------------------------------------------------------------------
// Shell executor type alias for dependency injection in tests
// ---------------------------------------------------------------------------

// A ShellExecutor is a callable that matches the signature of
// shell::ExecuteCommand.  Production code passes the real implementation;
// unit tests pass a stub that controls which commands succeed or fail.
using ShellExecutor = std::function<shell::ExecResult(
    const std::string& InCommand,
    const std::vector<std::string>& InArgs,
    shell::ExecMode InMode,
    std::optional<std::filesystem::path> InWorkingDir)>;

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct ExportOptions {
    std::filesystem::path outputDir;
    std::string format;          // "tar" or "zip"
    std::string prefix;          // empty = use repo name
    std::vector<std::string> includePaths; // repo-relative path filters; repeatable
    std::vector<std::string> excludePaths; // repo-relative path filters; repeatable
    bool includeSubmoduleStubs = false;
    bool noRecursive = false;
    bool dryRun = false;
    bool noMetadata = false;
    int revPad = 3;
    std::string source;          // "head" or "working-tree"
    bool single = false;
    int logCount = 10;
    bool validateReleaseArchive = true;      // Auto-run offline smoke for single root .tar archives when available.
    bool forceValidateReleaseArchive = false; // Fail when validation is requested but unavailable or failing.
    std::filesystem::path subtreePath;
    std::string exportName;
    bool keepSubtreePath = false;
    int splitSubrepoDepth = 0;
    bool includeSubrepos = false;
    bool allowMissingSubrepos = false;
};

struct ExportSubrepoManifestEntry {
    std::string path;
    std::string commit;
    std::string mode; // pointer-only | expanded
    bool contentIncluded = false;
    std::string remote;
    std::string status; // ok | failed
    std::string error;
};

struct ExportRecord {
    std::filesystem::path repoPath;
    std::string repoName;
    std::string relativeRepoPath;
    bool isRoot = false;
    bool exportAsSingle = false; // for split mode: this record exports as integrated single archive
    std::vector<std::filesystem::path> submodulePaths; // populated for root repo only
    bool isSubtree = false;
    std::filesystem::path subtreeAbsPath;
    std::filesystem::path subtreeRepoRelativePath;
    std::string subtreeDisplayPath;
};

struct ExportResult {
    std::string archiveName;     // e.g. "KTOStudio_rev042.tar"
    std::filesystem::path archivePath;
    bool success = false;
    std::string errorMessage;
    // Populated after successful export (used to build export-manifest.json)
    std::string sha256;
    std::uintmax_t sizeBytes = 0;
    std::string rootCommit;
    std::string rootBranch;
};

struct ExportUploadConfig {
    std::string target; // empty | local-sync-folder | rclone | gdrive-api
    std::filesystem::path localSyncFolder;
    std::string rcloneRemote;
    std::string rcloneDestination;
    std::string layout; // safe relative subdirectory under the sync root / remote destination
    bool copyManifest = false;
    bool copySha256 = false;
    bool returnUrl = true;
    std::string linkMode = "private"; // private | public-link
    std::string backend;
    std::string connectorHint;
    bool publicLink = false;
    bool yes = false;
};

struct ExportUploadConfigLayer {
    std::optional<std::string> target;
    std::optional<std::filesystem::path> localSyncFolder;
    std::optional<std::string> rcloneRemote;
    std::optional<std::string> rcloneDestination;
    std::optional<std::string> layout;
    std::optional<bool> copyManifest;
    std::optional<bool> copySha256;
    std::optional<bool> returnUrl;
    std::optional<std::string> linkMode;
    std::optional<std::string> backend;
    std::optional<std::string> connectorHint;
    std::optional<bool> publicLink;
    std::optional<bool> yes;
};

struct ExportUploadDoctorResult {
    bool ok = false;
    std::string status; // OK | MISSING_PATH | RCLONE_NOT_FOUND | RCLONE_REMOTE_MISSING | INVALID_CONFIG | NOT_CONFIGURED | FUTURE_BACKEND
    std::string backendLabel;
    bool thirdParty = false;
    std::string guidance;
    std::string output;
};

struct ExportUploadRequest {
    std::filesystem::path archivePath;
    std::filesystem::path manifestPath;
    std::string archiveName;
    ExportUploadConfig config;
};

struct ExportUploadResult {
    bool success = false;
    std::filesystem::path copiedArchivePath;
    std::filesystem::path copiedManifestPath;
    std::filesystem::path sha256SidecarPath;
    std::filesystem::path uploadManifestPath;
    std::filesystem::path localTargetPath;
    std::filesystem::path syncFolderPath;
    std::string remotePath;
    std::string remoteArchiveName;
    std::string sourceArchive;
    std::string sourceSha256;
    std::string backend;
    std::string connectorHint;
    std::optional<std::string> fileId;
    std::optional<std::string> webUrl;
    std::string visibility; // private | preserve | public-link
    bool permissionChanged = false;
    std::string urlStatus; // empty | URL_UNAVAILABLE
    std::string output;
    std::string errorMessage;
};

// ---------------------------------------------------------------------------
// Pure helper functions (no shell calls, no filesystem side-effects)
// ---------------------------------------------------------------------------

// Validates format, source, and rev-pad fields of ExportOptions.
// Returns true if all fields are valid; prints errors to stderr and returns
// false otherwise.
auto ValidateOptions(const ExportOptions& InOpts) -> bool;

// Computes the output directory: if InExplicit is non-empty, returns
// std::filesystem::path(InExplicit); otherwise returns
// InCwd / ".kano" / "tmp" / "git" / "export".
auto ResolveOutputDir(const std::filesystem::path& InCwd,
                      const std::string& InExplicit) -> std::filesystem::path;

// Formats a revision integer as a zero-padded string of at least InPad digits.
// Pure string formatting only — no shell calls.
auto FormatRevision(int InRevision, int InPad) -> std::string;

// Returns "<InRepoName>_rev<InRevision>.<InFormat>".
auto ComputeArchiveName(const std::string& InRepoName,
                        const std::string& InRevision,
                        const std::string& InFormat) -> std::string;

// Returns InExplicitPrefix if non-empty; otherwise returns "<InRepoName>/".
auto ComputePrefix(const std::string& InRepoName,
                   const std::string& InExplicitPrefix) -> std::string;

// Ensures archive prefixes always end with '/'. Empty input remains empty.
auto NormalizeArchivePrefix(const std::string& InPrefix) -> std::string;

// Returns the argument vector for:
//   git archive HEAD --format=<InFormat> --prefix=<InPrefix> --output=<InOutputPath>
auto BuildGitArchiveArgs(const std::string& InFormat,
                         const std::string& InPrefix,
                         const std::filesystem::path& InOutputPath) -> std::vector<std::string>;

// Returns the argument vector for subtree head exports.
// Strip mode uses:
//   git archive HEAD:<InRepoRelativeSubtree> --format=<InFormat> --prefix=<InPrefix> --output=<InOutputPath>
// Keep-path mode uses:
//   git archive HEAD --format=<InFormat> --prefix=<InPrefix> --output=<InOutputPath> <InRepoRelativeSubtree>
auto BuildGitArchiveArgsForSubtree(const std::string& InFormat,
                                   const std::string& InPrefix,
                                   const std::filesystem::path& InOutputPath,
                                   const std::string& InRepoRelativeSubtree,
                                   bool InKeepSubtreePath) -> std::vector<std::string>;

// Collects working-tree file paths under a subtree. Returned paths use '/'
// separators and are relative to the archive root (already stripped when
// InKeepSubtreePath=false).
auto CollectWorkingTreeFilesForSubtree(const std::filesystem::path& InRepoPath,
                                       const std::filesystem::path& InRepoRelativeSubtree,
                                       bool InKeepSubtreePath,
                                       const ShellExecutor& InExec) -> std::vector<std::string>;

// Returns the list of files to be included in a working-tree export,
// respecting .gitignore. If InSingle is true, also includes submodule contents.
auto CollectWorkingTreeFiles(const ExportRecord& InRecord,
                             const std::string& InPrefix,
                             bool InSingle,
                             const ShellExecutor& InExec) -> std::vector<std::string>;

// Returns "<InArchiveBaseName>_manifest.txt".
auto ComputeManifestName(const std::string& InArchiveBaseName) -> std::string;

// Returns "<InFilename>.sha256".
auto ComputeChecksumFilename(const std::string& InFilename) -> std::string;

// ---------------------------------------------------------------------------
// Export manifest JSON (machine-readable, for Jenkins pipeline consumption)
// ---------------------------------------------------------------------------

// Normalizes a host OS string to one of: "windows", "linux", "mac".
// Input may be any of the raw values returned by platform detection.
auto NormalizePlatform(const std::string& InRawPlatform) -> std::string;

// Returns the current host platform as a normalized string.
auto DetectHostPlatform() -> std::string;

// Data required to produce an export-manifest.json.
struct ExportManifestData {
    std::string schemaVersion = "1";
    std::string generator = "kog export";
    std::string projectName;
    std::string repository;
    std::string exportMode;       // "single" or "multi"
    bool singleArchive = false;
    std::string createdAt;        // ISO-8601 UTC
    std::string platform;         // normalized: windows / linux / mac
    std::string rootCommit;       // short SHA
    std::string rootBranch;
    std::string archiveFile;      // forward-slash path
    std::string format;           // "tar" or "zip"
    std::uintmax_t sizeBytes = 0;
    std::string sha256;
    std::vector<std::pair<std::string, std::string>> submodules; // {path, commit}
    std::vector<ExportSubrepoManifestEntry> subrepoEntries;
    std::string smokeTestResult;
    bool hasSubtree = false;
    std::string subtreeName;
    std::string subtreeSourcePath;
    std::string subtreeRepositoryPath;
    std::string subtreeRepoRelativePath;
    bool subtreeStripPath = true;
};

// Formats an ExportManifestData as a JSON string (no external dependencies).
auto FormatExportManifestJson(const ExportManifestData& InData) -> std::string;

// Reads the sha256 hex string from a .sha256 sidecar file produced by
// WriteChecksumFile.  Returns empty string on failure.
auto ReadSha256FromSidecar(const std::filesystem::path& InSidecarPath) -> std::string;

// Resolves export upload configuration using safe precedence:
// user/global < repo/local < CLI. Defaults must not enable upload or public links.
auto ResolveExportUploadConfig(const ExportUploadConfigLayer& InUserConfig,
                               const ExportUploadConfigLayer& InRepoConfig,
                               const ExportUploadConfigLayer& InCliConfig) -> ExportUploadConfig;

// Performs setup diagnostics for the selected upload target without mutating
// local files, remotes, permissions, OAuth state, or network resources.
auto DoctorExportUploadWithExecutor(const ExportUploadConfig& InConfig,
                                    const ShellExecutor& InExec) -> ExportUploadDoctorResult;

// Uploads/copies the archive and manifest to the configured target using only
// the injected shell executor for process work. Implementations must not call
// real rclone/network in tests; tests inject fake executors.
auto UploadExportArtifactsWithExecutor(const ExportUploadRequest& InRequest,
                                       const ShellExecutor& InExec) -> ExportUploadResult;

// Redacts token/password-like values from upload diagnostics and command output.
auto RedactExportUploadText(const std::string& InText) -> std::string;

// Formats the manifest text for one exported repo.
// InStatusOut and InLsFilesOut are the raw outputs of git status --short and
// git ls-files respectively (may be empty strings).
auto FormatManifest(const ExportRecord& InRecord,
                    const ExportResult& InResult,
                    const ExportOptions& InOpts,
                    const std::string& InStatusOut,
                    const std::string& InLsFilesOut) -> std::string;

// Formats the human-readable metadata markdown text.
auto FormatMarkdownMetadata(const ExportRecord& InRecord,
                            const ExportResult& InResult,
                            const ExportOptions& InOpts,
                            const std::string& InRootLog,
                            const std::vector<std::pair<std::string, std::string>>& InSubLogs) -> std::string;

// Formats a single progress line: "Exported <InRepoName> -> <InArchivePath>".
auto FormatProgressLine(const std::string& InRepoName,
                        const std::filesystem::path& InArchivePath) -> std::string;

// Formats the final summary block listing output dir, metadata dir, and all
// successful archive names.
auto FormatSummary(const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const std::vector<ExportResult>& InResults) -> std::string;

// Formats the dry-run plan: lists each repo's computed archive name and the
// resolved output directory.
auto FormatDryRunPlan(const std::vector<ExportRecord>& InRecords,
                      const ExportOptions& InOpts) -> std::string;

// Builds the ordered export list: workspace root first, then discovered
// subrepos (unless InNoRecursive is true).
// InRoot is the workspace root path; InDiscovered is the list returned by
// DiscoverWorkspaceRepos (may include the root or not).
auto BuildExportList(const std::filesystem::path& InRoot,
                       const std::vector<workspace::RepoRecord>& InDiscovered,
                       bool InNoRecursive,
                       bool InSingle,
                       int InSplitSubrepoDepth,
                       const ShellExecutor& InExec) -> std::vector<ExportRecord>;

// Returns true if the given path is marked with 'export-ignore' attribute
// in the context of InRepoPath.
auto IsPathIgnoredByExport(const std::filesystem::path& InRepoPath,
                           const std::filesystem::path& InTargetPath,
                           const ShellExecutor& InExec) -> bool;

// ---------------------------------------------------------------------------
// Shell-calling helpers (exposed for unit testing via dependency injection)
// ---------------------------------------------------------------------------

// Exports a single repository.  InExec is the shell executor to use for all
// external process invocations; pass shell::ExecuteCommand in production and
// a stub in tests.
auto ExportOneRepo(const ExportRecord& InRecord,
                   const ExportOptions& InOpts,
                   const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const ShellExecutor& InExec) -> ExportResult;

// Top-level export orchestrator.  InExec is the shell executor to use for all
// external process invocations; pass shell::ExecuteCommand in production and
// a stub in tests.  InExportList is the pre-built list of repos to export
// (allows tests to bypass DiscoverWorkspaceRepos).
// Returns 0 on full success, 1 if any repo failed.
auto RunExportWithExecutor(const ExportOptions& InOpts,
                           const std::vector<ExportRecord>& InExportList,
                           const std::filesystem::path& InOutputDir,
                           const std::filesystem::path& InMetadataDir,
                           const ShellExecutor& InExec) -> int;

// ---------------------------------------------------------------------------
// CLI registration (public entry point)
// ---------------------------------------------------------------------------

// Registers the 'export' subcommand with CLI11.
// Declared here so that test targets can call it without depending on
// command_declarations.hpp (which lives in the runtime layer).
void RegisterExport(CLI::App& InApp);

} // namespace kano::git::commands
