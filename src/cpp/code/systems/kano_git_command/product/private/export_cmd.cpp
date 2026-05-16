// export_cmd.cpp — kog export command implementation
//
// Packages a multi-repo workspace into a set of archive files, one per
// repository. Follows the anonymous-namespace helper pattern from status_cmd.cpp.

#include "export_helpers.hpp"

#include <CLI/CLI.hpp>
#include "discovery.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace kano::git::commands {

// ---------------------------------------------------------------------------
// Pure helper function implementations
// ---------------------------------------------------------------------------

auto ValidateOptions(const ExportOptions& InOpts) -> bool {
    bool ok = true;
    if (InOpts.format != "tar" && InOpts.format != "zip") {
        std::cerr << "kog export: invalid --format value '" << InOpts.format
                  << "'; must be 'tar' or 'zip'\n";
        ok = false;
    }
    if (InOpts.source != "head" && InOpts.source != "working-tree") {
        std::cerr << "kog export: invalid --source value '" << InOpts.source
                  << "'; must be 'head' or 'working-tree'\n";
        ok = false;
    }
    if (InOpts.revPad <= 0) {
        std::cerr << "kog export: invalid --rev-pad value " << InOpts.revPad
                  << "; must be a positive integer\n";
        ok = false;
    }
    if (!InOpts.validateReleaseArchive && InOpts.forceValidateReleaseArchive) {
        std::cerr << "kog export: --validate-release-archive conflicts with "
                  << "--no-validate-release-archive\n";
        ok = false;
    }
    return ok;
}

auto ResolveOutputDir(const std::filesystem::path& InCwd,
                      const std::string& InExplicit) -> std::filesystem::path {
    if (!InExplicit.empty()) {
        return std::filesystem::path(InExplicit);
    }
    return InCwd / ".kano" / "tmp" / "git" / "export";
}

auto FormatRevision(int InRevision, int InPad) -> std::string {
    std::ostringstream oss;
    oss << std::setw(InPad) << std::setfill('0') << InRevision;
    return oss.str();
}

auto ComputeArchiveName(const std::string& InRepoName,
                        const std::string& InRevision,
                        const std::string& InFormat) -> std::string {
    return InRepoName + "_rev" + InRevision + "." + InFormat;
}

auto ComputePrefix(const std::string& InRepoName,
                   const std::string& InExplicitPrefix) -> std::string {
    if (!InExplicitPrefix.empty()) {
        return InExplicitPrefix;
    }
    return InRepoName + "/";
}

auto BuildGitArchiveArgs(const std::string& InFormat,
                         const std::string& InPrefix,
                         const std::filesystem::path& InOutputPath) -> std::vector<std::string> {
    return {
        "archive",
        "HEAD",
        "--format=" + InFormat,
        "--prefix=" + InPrefix,
        "--output=" + InOutputPath.generic_string()
    };
}


auto CollectWorkingTreeFiles(const ExportRecord& InRecord, bool InSingle, const ShellExecutor& InExec) -> std::vector<std::string> {
    const std::string repoDirName = InRecord.repoPath.filename().generic_string();
    std::vector<std::string> allFiles;

    auto getFiles = [&](const std::filesystem::path& repoPath, const std::string& prefix) {
        const auto res = InExec("git", {"ls-files", "--cached", "--others", "--exclude-standard"}, shell::ExecMode::Capture, repoPath);
        if (res.exitCode == 0) {
            std::istringstream iss(res.stdoutStr);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) {
                    allFiles.push_back(prefix + line);
                }
            }
        }
    };

    // Root repo files
    getFiles(InRecord.repoPath, repoDirName + "/");

    // If single mode, recursively add submodule files
    if (InSingle && InRecord.isRoot) {
        for (const auto& subRelPath : InRecord.submodulePaths) {
            const std::string subPrefix = repoDirName + "/" + subRelPath.generic_string() + "/";
            getFiles(InRecord.repoPath / subRelPath, subPrefix);
        }
    }

    return allFiles;
}

auto ComputeManifestName(const std::string& InArchiveBaseName) -> std::string {
    return InArchiveBaseName + "_manifest.txt";
}

auto ComputeChecksumFilename(const std::string& InFilename) -> std::string {
    return InFilename + ".sha256";
}

namespace {

auto CurrentUtcIso8601Export() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utcTm{};
#if defined(_WIN32)
    gmtime_s(&utcTm, &tt);
#else
    gmtime_r(&tt, &utcTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utcTm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

auto TrimExport(std::string InValue) -> std::string {
    while (!InValue.empty() &&
           (InValue.back() == '\n' || InValue.back() == '\r' ||
            InValue.back() == ' ' || InValue.back() == '\t')) {
        InValue.pop_back();
    }
    std::size_t start = 0;
    while (start < InValue.size() &&
           (InValue[start] == ' ' || InValue[start] == '\t')) {
        start += 1;
    }
    return InValue.substr(start);
}

auto RelativeDisplayPath(const std::filesystem::path& InRoot, const std::filesystem::path& InPath) -> std::filesystem::path {
    const auto normalizedRoot = InRoot.lexically_normal();
    const auto normalizedPath = InPath.lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (!relative.empty()) {
        return relative;
    }
    return normalizedPath;
}

auto GroupFromRelativePath(const std::filesystem::path& InRelativePath) -> std::string {
    const auto parent = InRelativePath.parent_path().generic_string();
    if (parent.empty() || parent == ".") {
        return ".";
    }
    return parent;
}

auto FirstNLines(const std::string& InText, int InN) -> std::string {
    std::istringstream iss(InText);
    std::string line;
    std::ostringstream out;
    int count = 0;
    while (count < InN && std::getline(iss, line)) {
        out << line << "\n";
        count += 1;
    }
    return out.str();
}

} // anonymous namespace

auto FormatManifest(const ExportRecord& InRecord,
                    const ExportResult& InResult,
                    const ExportOptions& InOpts,
                    const std::string& InStatusOut,
                    const std::string& InLsFilesOut) -> std::string {
    const std::string archiveBase = InResult.archiveName.substr(
        0, InResult.archiveName.rfind('.'));
    const std::string prefix = ComputePrefix(InRecord.repoName, InOpts.prefix);

    std::ostringstream oss;
    oss << "Package: " << archiveBase << "\n";
    oss << "Repo: " << InRecord.repoPath.filename().generic_string() << "\n";
    oss << "RepoRoot: " << InRecord.repoPath.generic_string() << "\n";
    oss << "ArchiveRoot: " << prefix << "\n";
    oss << "RevisionFirstParentCount: " << "\n";
    oss << "GitShortSHA: " << "\n";
    oss << "Format: " << InOpts.format << "\n";
    oss << "CreatedAtUTC: " << CurrentUtcIso8601Export() << "\n";
    oss << "\n";
    oss << "GitStatusShort:\n";
    oss << TrimExport(InStatusOut) << "\n";
    oss << "\n";
    oss << "TrackedFilesSample:\n";
    oss << FirstNLines(InLsFilesOut, 200);
    return oss.str();
}

auto FormatMarkdownMetadata(const ExportRecord& InRecord,
                           const ExportResult& InResult,
                           const ExportOptions& InOpts,
                           const std::string& InRootLogOut,
                           const std::vector<std::pair<std::string, std::string>>& InSubLogs) -> std::string {
    const std::string archiveBase = InResult.archiveName.substr(
        0, InResult.archiveName.rfind('.'));

    std::ostringstream oss;
    oss << "# Export Metadata: " << archiveBase << "\n\n";
    oss << "## General Info\n";
    oss << "- **Repository**: `" << InRecord.repoName << "`\n";
    oss << "- **Source Path**: `" << InRecord.repoPath.generic_string() << "`\n";
    oss << "- **Exported At**: " << CurrentUtcIso8601Export() << "\n";
    oss << "- **Format**: " << InOpts.format << "\n";
    oss << "- **Archive File**: `" << InResult.archiveName << "`\n\n";

    oss << "## Project History (last " << InOpts.logCount << " entries)\n";
    oss << "### Root Repository: " << InRecord.repoName << "\n";
    oss << "```text\n";
    if (InRootLogOut.empty()) {
        oss << "(No history available)\n";
    } else {
        oss << InRootLogOut;
    }
    oss << "\n```\n\n";

    for (const auto& [subPath, subLog] : InSubLogs) {
        oss << "### Submodule: " << subPath << "\n";
        oss << "```text\n";
        if (subLog.empty()) {
            oss << "(No history available)\n";
        } else {
            oss << subLog;
        }
        oss << "\n```\n\n";
    }

    return oss.str();
}

auto FormatProgressLine(const std::string& InDisplayName,
                        const std::filesystem::path& InArchivePath) -> std::string {
    return "  Exported " + InDisplayName + " -> " + InArchivePath.generic_string();
}

auto FormatSummary(const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const std::vector<ExportResult>& InResults) -> std::string {
    std::ostringstream oss;
    oss << "Export complete\n";
    oss << "OutputDir: " << InOutputDir.generic_string() << "\n";
    oss << "MetadataDir: " << InMetadataDir.generic_string() << "\n";
    oss << "Archives:\n";
    for (const auto& result : InResults) {
        if (result.success) {
            oss << "  " << result.archiveName << "\n";
        }
    }
    return oss.str();
}

auto FormatDryRunPlan(const std::vector<ExportRecord>& InRecords,
                       const ExportOptions& InOpts) -> std::string {
    std::ostringstream oss;
    oss << "Dry-run export plan\n";
    oss << "OutputDir: " << InOpts.outputDir.generic_string() << "\n";
    oss << "Repos to export:\n";
    std::string currentGroup;
    for (const auto& record : InRecords) {
        const auto relativePath = std::filesystem::path(record.relativeRepoPath);
        const auto group = GroupFromRelativePath(relativePath);
        if (group != currentGroup) {
            currentGroup = group;
            oss << "\nGROUP: " << currentGroup << "\n";
        }
        // Use a placeholder revision for dry-run display
        const std::string archiveName = ComputeArchiveName(
            record.repoName, FormatRevision(0, InOpts.revPad), InOpts.format);
        oss << "  " << record.repoName << " (" << record.relativeRepoPath << ") -> " << archiveName << "\n";
    }
    return oss.str();
}

auto IsPathIgnoredByExport(const std::filesystem::path& InRepoPath,
                           const std::filesystem::path& InTargetPath,
                           const ShellExecutor& InExec) -> bool {
    const auto relativePath = std::filesystem::relative(InTargetPath, InRepoPath);
    if (relativePath.empty()) {
        return false;
    }
    const auto result = InExec("git", {"check-attr", "export-ignore", relativePath.generic_string()},
                               shell::ExecMode::Capture, InRepoPath);
    if (result.exitCode != 0) {
        return false;
    }
    // Expected output: <path>: export-ignore: set
    return result.stdoutStr.find("export-ignore: set") != std::string::npos;
}

auto BuildExportList(const std::filesystem::path& InRoot,
                      const std::vector<workspace::RepoRecord>& InDiscovered,
                      bool InNoRecursive,
                      bool InSingle,
                      const ShellExecutor& InExec) -> std::vector<ExportRecord> {
    std::vector<ExportRecord> result;

    // Always prepend the workspace root as the first entry
    ExportRecord rootRecord;
    rootRecord.repoPath = InRoot;
    rootRecord.repoName = InRoot.filename().generic_string();
    if (rootRecord.repoName.empty()) {
        rootRecord.repoName = InRoot.lexically_normal().filename().generic_string();
    }
    const std::string rootName = rootRecord.repoName;

    rootRecord.isRoot = true;
    const auto normalizedRoot = InRoot.lexically_normal();

    // Collect submodule paths for exclusion (multi-repo) or history (single-repo)
    for (const auto& repo : InDiscovered) {
        const auto normalizedPath = repo.path.lexically_normal();
        bool isRoot = false;
        std::error_code ec;
        if (std::filesystem::exists(normalizedPath, ec) && std::filesystem::exists(normalizedRoot, ec)) {
            if (std::filesystem::equivalent(normalizedPath, normalizedRoot, ec)) {
                isRoot = true;
            }
        }
        if (!isRoot && normalizedPath == normalizedRoot) {
            isRoot = true;
        }

        if (!isRoot) {
            rootRecord.submodulePaths.push_back(RelativeDisplayPath(InRoot, repo.path));
        }
    }
    std::sort(rootRecord.submodulePaths.begin(), rootRecord.submodulePaths.end());

    result.push_back(std::move(rootRecord));

    if (InNoRecursive || InSingle) {
        return result;
    }

    // Append discovered subrepos (skip any that match the root path)
    for (const auto& repo : InDiscovered) {
        const auto normalizedPath = repo.path.lexically_normal();
        std::error_code ec;
        bool isRoot = false;
        if (std::filesystem::exists(normalizedPath, ec) && std::filesystem::exists(normalizedRoot, ec)) {
            if (std::filesystem::equivalent(normalizedPath, normalizedRoot, ec)) {
                isRoot = true;
            }
        }
        if (!isRoot && normalizedPath == normalizedRoot) {
            isRoot = true;
        }

        if (isRoot) {
            continue;
        }

        // Check if the repo path is ignored by the root repo
        if (IsPathIgnoredByExport(normalizedRoot, normalizedPath, InExec)) {
            continue;
        }

        ExportRecord rec;
        rec.repoPath = repo.path;
        rec.relativeRepoPath = RelativeDisplayPath(InRoot, repo.path).generic_string();

        // Include root repo name in subrepo names for clarity
        std::string subName = rec.relativeRepoPath;
        std::replace(subName.begin(), subName.end(), '/', '_');
        std::replace(subName.begin(), subName.end(), '\\', '_');
        rec.repoName = rootName + "_" + subName;

        rec.isRoot = false;
        result.push_back(std::move(rec));
    }

    // Sort by relative path to ensure consistent grouping
    std::sort(result.begin(), result.end(), [](const ExportRecord& A, const ExportRecord& B) {
        return A.relativeRepoPath < B.relativeRepoPath;
    });

    return result;
}

// ---------------------------------------------------------------------------
// Shell-calling helpers (implemented in task 4)
// ---------------------------------------------------------------------------

namespace {

auto GitCapture(const std::filesystem::path& InRepo,
                const std::vector<std::string>& InArgs,
                const ShellExecutor& InExec) -> shell::ExecResult {
    return InExec("git", InArgs, shell::ExecMode::Capture, InRepo);
}

auto ComputeRevision(const std::filesystem::path& InRepoPath, int InRevPad,
                     const ShellExecutor& InExec) -> std::string {
    const auto result = GitCapture(InRepoPath, {"rev-list", "--count", "--first-parent", "HEAD"}, InExec);
    if (result.exitCode != 0 || result.stdoutStr.empty()) {
        return FormatRevision(0, InRevPad);
    }
    try {
        const int rev = std::stoi(result.stdoutStr);
        return FormatRevision(rev, InRevPad);
    } catch (const std::exception&) {
        return FormatRevision(0, InRevPad);
    }
}

auto WriteChecksumFile(const std::filesystem::path& InTargetFile,
                       const std::filesystem::path& InMetadataDir,
                       const ShellExecutor& InExec) -> bool {
    const auto filename = InTargetFile.filename().generic_string();
    const auto checksumPath = InMetadataDir / ComputeChecksumFilename(filename);

    // Try sha256sum (Linux, Git Bash)
    {
        const auto r = InExec(
            "sha256sum", {InTargetFile.generic_string()},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    // Try shasum -a 256 (macOS)
    {
        const auto r = InExec(
            "shasum", {"-a", "256", InTargetFile.generic_string()},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    // Try PowerShell Get-FileHash (Windows fallback)
    {
        const auto r = InExec(
            "powershell",
            {"-NoProfile", "-Command",
             "Get-FileHash -Algorithm SHA256 '" + InTargetFile.generic_string() + "' | Select-Object -ExpandProperty Hash"},
            shell::ExecMode::Capture, InMetadataDir);
        if (r.exitCode == 0 && !r.stdoutStr.empty()) {
            std::ofstream f(checksumPath);
            if (f) {
                f << r.stdoutStr;
                return true;
            }
        }
    }

    std::cerr << "kog export: warning: no sha256 tool found; skipping checksum for "
              << InTargetFile.filename().generic_string() << "\n";
    return false;
}

} // anonymous namespace

namespace {

auto ResolveReleaseArchiveSmokeScript(const std::filesystem::path& InWorkspaceRoot) -> std::filesystem::path {
    const auto scriptPath = InWorkspaceRoot / "src" / "shell" / "test" / "smoke-release-archive.sh";
    std::error_code ec;
    if (std::filesystem::exists(scriptPath, ec) && std::filesystem::is_regular_file(scriptPath, ec)) {
        return scriptPath;
    }
    return {};
}

auto HasBash(const std::filesystem::path& InWorkspaceRoot,
             const ShellExecutor& InExec) -> bool {
    const auto result = InExec("bash", {"-lc", "true"}, shell::ExecMode::Capture, InWorkspaceRoot);
    return result.exitCode == 0;
}

auto AppendCapturedOutput(std::ostream& InStream,
                          const std::string& InLabel,
                          const std::string& InText) -> void {
    const auto trimmed = TrimExport(InText);
    if (!trimmed.empty()) {
        InStream << InLabel << ":\n" << trimmed << "\n";
    }
}

auto RunReleaseArchiveValidation(const ExportRecord& InRecord,
                                 const ExportResult& InResult,
                                 const ExportOptions& InOpts,
                                 const std::filesystem::path& InWorkspaceRoot,
                                 const ShellExecutor& InExec) -> int {
    if (!InOpts.validateReleaseArchive) {
        return 0;
    }

    if (!InRecord.isRoot) {
        return 0;
    }

    auto skipOrFail = [&](const std::string& InReason) -> int {
        if (InOpts.forceValidateReleaseArchive) {
            std::cerr << "kog export: release archive validation required but unavailable: "
                      << InReason << "\n";
            return 1;
        }
        std::cout << "  Skipped release archive validation: " << InReason << "\n";
        return 0;
    };

    if (!InResult.success) {
        return 0;
    }

    if (InOpts.format != "tar" || InResult.archivePath.extension() != ".tar") {
        return skipOrFail("only .tar archives are supported by smoke-release-archive.sh");
    }

    if (!InOpts.single) {
        return skipOrFail("requires --single so submodule working-tree contents are included");
    }

    const auto smokeScript = ResolveReleaseArchiveSmokeScript(InWorkspaceRoot);
    if (smokeScript.empty()) {
        return skipOrFail("src/shell/test/smoke-release-archive.sh was not found");
    }

    if (!HasBash(InWorkspaceRoot, InExec)) {
        return skipOrFail("bash is not available on PATH");
    }

    std::cout << "  Validating release archive -> " << InResult.archivePath.generic_string() << "\n";
    const auto validation = InExec(
        "bash",
        {smokeScript.generic_string(), InResult.archivePath.generic_string()},
        shell::ExecMode::Capture,
        InWorkspaceRoot);

    if (validation.exitCode != 0) {
        std::cerr << "kog export: release archive validation failed for "
                  << InResult.archivePath.generic_string() << "\n";
        AppendCapturedOutput(std::cerr, "stdout", validation.stdoutStr);
        AppendCapturedOutput(std::cerr, "stderr", validation.stderrStr);
        return validation.exitCode == 0 ? 1 : validation.exitCode;
    }

    AppendCapturedOutput(std::cout, "  release-smoke", validation.stdoutStr);
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public shell-calling helpers (dependency-injected, exposed for unit tests)
// ---------------------------------------------------------------------------

auto ExportOneRepo(const ExportRecord& InRecord,
                   const ExportOptions& InOpts,
                   const std::filesystem::path& InOutputDir,
                   const std::filesystem::path& InMetadataDir,
                   const ShellExecutor& InExec) -> ExportResult {
    ExportResult result;
    result.success = false;

    const std::string revision = ComputeRevision(InRecord.repoPath, InOpts.revPad, InExec);
    result.archiveName = ComputeArchiveName(InRecord.repoName, revision, InOpts.format);
    result.archivePath = InOutputDir / result.archiveName;

    // Run archive
    shell::ExecResult archiveResult;
    if (InOpts.source == "head") {
        const std::string prefix = ComputePrefix(InRecord.repoName, InOpts.prefix);
        const auto args = BuildGitArchiveArgs(InOpts.format, prefix, result.archivePath);
        archiveResult = InExec("git", args, shell::ExecMode::Capture, InRecord.repoPath);
    } else {
        // working-tree: Collect files via git ls-files to respect .gitignore
        const auto fileList = CollectWorkingTreeFiles(InRecord, InOpts.single, InExec);
        if (fileList.empty()) {
            result.errorMessage = "no files found to export in working-tree";
            return result;
        }

        const auto listPath = InMetadataDir / (InRecord.repoName + "_files.txt");
        {
            std::ofstream lf(listPath);
            for (const auto& f : fileList) {
                lf << f << "\n";
            }
        }

        std::vector<std::string> args;
        args.push_back("-czf");
        args.push_back(result.archivePath.generic_string());
        args.push_back("-C");
        args.push_back(InRecord.repoPath.parent_path().generic_string());
        args.push_back("-T");
        args.push_back(listPath.generic_string());

        archiveResult = InExec("tar", args, shell::ExecMode::Capture, InRecord.repoPath);
        std::error_code ec;
        std::filesystem::remove(listPath, ec);
    }

    if (archiveResult.exitCode != 0) {
        result.errorMessage = "archive failed: " + archiveResult.stderrStr;
        return result;
    }

    result.success = true;

    if (InOpts.noMetadata) {
        return result;
    }

    // Write manifest
    const auto statusOut = GitCapture(InRecord.repoPath, {"status", "--short"}, InExec);
    const auto lsFilesOut = GitCapture(InRecord.repoPath, {"ls-files"}, InExec);
    const std::string manifestText = FormatManifest(
        InRecord, result, InOpts,
        statusOut.stdoutStr, lsFilesOut.stdoutStr);

    const std::string archiveBase = result.archiveName.substr(
        0, result.archiveName.rfind('.'));
    const auto manifestName = ComputeManifestName(archiveBase);
    const auto manifestPath = InMetadataDir / manifestName;

    std::ofstream mf(manifestPath);
    if (mf) {
        mf << manifestText;
    }

    // Write checksums
    WriteChecksumFile(result.archivePath, InMetadataDir, InExec);
    WriteChecksumFile(manifestPath, InMetadataDir, InExec);

    // Write human/agent metadata markdown file (alongside archive)
    const auto logArgs = std::vector<std::string>{"log", "-n", std::to_string(InOpts.logCount), "--oneline", "--decorate"};
    const auto rootLogOut = GitCapture(InRecord.repoPath, logArgs, InExec);

    std::vector<std::pair<std::string, std::string>> subLogs;
    if (InOpts.single && InRecord.isRoot) {
        for (const auto& subRelPath : InRecord.submodulePaths) {
            const auto subLog = GitCapture(InRecord.repoPath / subRelPath, logArgs, InExec);
            subLogs.push_back({subRelPath.generic_string(), subLog.stdoutStr});
        }
    }

    const std::string mdText = FormatMarkdownMetadata(InRecord, result, InOpts, rootLogOut.stdoutStr, subLogs);
    const auto mdPath = InOutputDir / (archiveBase + ".md");
    std::ofstream mdf(mdPath);
    if (mdf) {
        mdf << mdText;
    }

    return result;
}

auto RunExportWithExecutor(const ExportOptions& InOpts,
                           const std::vector<ExportRecord>& InExportList,
                           const std::filesystem::path& InOutputDir,
                           const std::filesystem::path& InMetadataDir,
                           const ShellExecutor& InExec) -> int {
    std::vector<ExportResult> results;
    bool anyFailed = false;

    std::string currentGroup;
    for (const auto& record : InExportList) {
        const auto relativePath = std::filesystem::path(record.relativeRepoPath);
        const auto group = GroupFromRelativePath(relativePath);
        if (group != currentGroup) {
            currentGroup = group;
            std::cout << "\nGROUP: " << currentGroup << "\n";
        }

        const auto result = ExportOneRepo(record, InOpts, InOutputDir, InMetadataDir, InExec);
        if (!result.success) {
            std::cerr << "kog export: failed to export '" << record.repoName
                      << "' (" << record.relativeRepoPath << "): " << result.errorMessage << "\n";
            anyFailed = true;
        } else {
            const std::string displayName = record.isRoot ? record.repoName : record.relativeRepoPath;
            std::cout << FormatProgressLine(displayName, result.archivePath) << "\n";
            const int validationExit = RunReleaseArchiveValidation(record, result, InOpts, InExportList.front().repoPath, InExec);
            if (validationExit != 0) {
                anyFailed = true;
            }
        }
        results.push_back(result);
    }

    std::cout << "\n" << FormatSummary(InOutputDir, InMetadataDir, results);

    return anyFailed ? 1 : 0;
}

namespace {

auto RunExport(const ExportOptions& InOpts) -> int {
    if (!ValidateOptions(InOpts)) {
        return 1;
    }

    const auto cwd = std::filesystem::current_path();

    // Real shell executor wraps shell::ExecuteCommand
    ShellExecutor realExec = [](const std::string& cmd,
                                const std::vector<std::string>& args,
                                shell::ExecMode mode,
                                std::optional<std::filesystem::path> workDir) {
        return shell::ExecuteCommand(cmd, args, mode, workDir);
    };

    // Validate workspace root is a git repo
    const auto gitCheck = realExec(
        "git", {"rev-parse", "--git-dir"},
        shell::ExecMode::Capture, cwd);
    if (gitCheck.exitCode != 0) {
        std::cerr << "kog export: current directory is not a git repository\n";
        return 1;
    }

    // Discover subrepos
    workspace::DiscoverOptions discoverOpts;
    discoverOpts.rootDir = cwd;
    discoverOpts.scope = workspace::DiscoverScope::RegisteredOnly;
    const auto discovery = workspace::DiscoverRepos(discoverOpts);

    const auto outputDir = InOpts.outputDir.empty()
        ? ResolveOutputDir(cwd, "")
        : InOpts.outputDir;
    const auto metadataDir = outputDir / "metadata";

    const auto exportList = BuildExportList(cwd, discovery.repos, InOpts.noRecursive, InOpts.single, realExec);

    if (InOpts.dryRun) {
        ExportOptions optsWithDir = InOpts;
        optsWithDir.outputDir = outputDir;
        std::cout << FormatDryRunPlan(exportList, optsWithDir);
        return 0;
    }

    // Create output directories
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        std::cerr << "kog export: failed to create output directory: " << ec.message() << "\n";
        return 1;
    }
    std::filesystem::create_directories(metadataDir, ec);
    if (ec) {
        std::cerr << "kog export: failed to create metadata directory: " << ec.message() << "\n";
        return 1;
    }

    return RunExportWithExecutor(InOpts, exportList, outputDir, metadataDir, realExec);
}

} // anonymous namespace

} // namespace kano::git::commands

// ---------------------------------------------------------------------------
// Public registration function (task 5)
// ---------------------------------------------------------------------------

namespace kano::git::commands {

void RegisterExport(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("export",
        "Package a multi-repo workspace into archive files, one per repository");

    auto* outputDir = new std::string{};
    auto* format = new std::string{"tar"};
    auto* prefix = new std::string{};
    auto* includeSubmoduleStubs = new bool{false};
    auto* noRecursive = new bool{false};
    auto* dryRun = new bool{false};
    auto* noMetadata = new bool{false};
    auto* revPad = new int{3};
    auto* source = new std::string{"head"};
    auto* single = new bool{false};
    auto* logCount = new int{10};
    auto* noValidateReleaseArchive = new bool{false};
    auto* forceValidateReleaseArchive = new bool{false};

    cmd->add_option("--output", *outputDir,
        "Output directory for archives (default: <cwd>/.kano/tmp/git/export/)");
    cmd->add_option("--format", *format,
        "Archive format: tar|zip (default: tar)");
    cmd->add_option("--prefix", *prefix,
        "Override archive root prefix (default: <repo-name>/)");
    cmd->add_flag("--include-submodule-stubs", *includeSubmoduleStubs,
        "Include empty submodule placeholder dirs in root archive (working-tree only)");
    cmd->add_flag("--no-recursive", *noRecursive,
        "Export only the workspace root repo, skip subrepos");
    cmd->add_flag("--dry-run", *dryRun,
        "Print what would be exported without creating files");
    cmd->add_flag("--no-metadata", *noMetadata,
        "Skip manifest and SHA-256 generation");
    cmd->add_option("--rev-pad", *revPad,
        "Revision zero-padding width (default: 3)");
    cmd->add_option("--source", *source,
        "Archive source: head|working-tree (default: head)");
    cmd->add_flag("--single", *single,
        "Export the entire workspace into a single archive (implies --no-recursive)");
    cmd->add_option("--log-count", *logCount,
        "Number of history log entries to include in metadata (default: 10)");
    cmd->add_flag("--no-validate-release-archive", *noValidateReleaseArchive,
        "Skip automatic smoke validation for single-file release archives");
    cmd->add_flag("--validate-release-archive", *forceValidateReleaseArchive,
        "Require smoke validation for the root release archive; fail if it cannot run");

    cmd->callback([=]() {
        ExportOptions opts;
        opts.outputDir = *outputDir;
        opts.format = *format;
        opts.prefix = *prefix;
        opts.includeSubmoduleStubs = *includeSubmoduleStubs;
        opts.noRecursive = *noRecursive;
        opts.dryRun = *dryRun;
        opts.noMetadata = *noMetadata;
        opts.revPad = *revPad;
        opts.source = *source;
        opts.single = *single;
        opts.logCount = *logCount;
        opts.validateReleaseArchive = !*noValidateReleaseArchive;
        opts.forceValidateReleaseArchive = *forceValidateReleaseArchive;

        // If --single is specified, default --source to "working-tree" unless explicitly overridden.
        if (opts.single && cmd->get_option("--source")->results().empty()) {
            opts.source = "working-tree";
        }

        const int exitCode = RunExport(opts);
        if (exitCode != 0) {
            throw CLI::RuntimeError(exitCode);
        }
    });
}

} // namespace kano::git::commands
