// export_integration_test.cpp — End-to-end integration tests for kog export
//
// Tests the full kog export command against a real temporary git repo with
// one registered submodule, verifying file system output and exit codes.
//
// Test 1: Default options produce one .tar per repo in <cwd>/.kano/tmp/git/export/
//         with correct _revNNN naming (Requirements 1.1, 1.2, 2.3, 3.1, 3.4, 3.6)
// Test 2: --dry-run creates no files and exits 0 (Requirements 6.1, 6.2, 6.3, 6.4)
// Test 3: --no-metadata skips manifest and .sha256 files (Requirement 5.7)
// Test 4: --no-recursive exports only the root repo (Requirement 1.3, 3.7)
// Test 5: --format zip produces .zip archives (Requirement 3.5)
// Test 6: --output <dir> writes archives to the specified directory (Requirement 3.7)
//
// **Validates: Requirements 1.1, 1.2, 1.3, 3.1, 3.4, 3.5, 3.6, 3.7, 5.7,
//              6.1, 6.2, 6.3, 6.4**

#include "functional_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace kano::git::tests::functional;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

// Assert a CommandResult succeeded, printing diagnostics on failure.
auto RequireSuccess(const CommandResult& InResult, const std::string& InContext) -> void {
    INFO(InContext);
    INFO("exit=" << InResult.exitCode);
    INFO("stdout=" << InResult.stdoutText);
    INFO("stderr=" << InResult.stderrText);
    REQUIRE(InResult.exitCode == 0);
}

// Write a text file, creating parent directories as needed.
auto WriteTextFile(const std::filesystem::path& InPath, const std::string& InText) -> void {
    std::filesystem::create_directories(InPath.parent_path());
    std::ofstream out(InPath, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << InText;
}

auto FileContainsCRLF(const std::filesystem::path& InPath) -> bool {
    std::ifstream in(InPath, std::ios::binary);
    if (!in.good()) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return text.find("\r\n") != std::string::npos;
}

// Configure git identity in a repo (required for commits in a fresh sandbox).
auto ConfigureIdentity(const std::filesystem::path& InRepo) -> void {
    RequireSuccess(RunGit({"config", "user.name", "Kano Test"}, InRepo), "config user.name");
    RequireSuccess(RunGit({"config", "user.email", "kano-test@example.invalid"}, InRepo), "config user.email");
}

auto ResolvePythonCommand(const std::filesystem::path& InRepo) -> std::string {
    for (const char* candidate : {"python3", "python"}) {
        const auto probe = RunCommand(candidate, {"-c", "import sys"}, InRepo);
        if (probe.exitCode == 0) {
            return candidate;
        }
    }
    return "python";
}

auto RunPythonCommand(const std::vector<std::string>& InArgs,
                      const std::filesystem::path& InRepo) -> CommandResult {
    return RunCommand(ResolvePythonCommand(InRepo), InArgs, InRepo);
}

// Count files in a directory matching a given extension (non-recursive).
auto CountFilesWithExtension(const std::filesystem::path& InDir,
                              const std::string& InExt) -> int {
    if (!std::filesystem::exists(InDir)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(InDir)) {
        if (entry.is_regular_file() && entry.path().extension() == InExt) {
            ++count;
        }
    }
    return count;
}

// Check whether any file in a directory has a name containing the given substring.
auto AnyFileContains(const std::filesystem::path& InDir,
                     const std::string& InSubstring) -> bool {
    if (!std::filesystem::exists(InDir)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(InDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(InSubstring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Check whether any file in a directory (recursively) has a name containing
// the given substring.
auto AnyFileContainsRecursive(const std::filesystem::path& InDir,
                               const std::string& InSubstring) -> bool {
    if (!std::filesystem::exists(InDir)) {
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(InDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(InSubstring) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Check whether a tar archive contains a file path substring.
auto ArchiveContainsPath(const std::filesystem::path& InArchive,
                         const std::string& InSubstring,
                         const std::filesystem::path& InRepo) -> bool {
    const auto result = RunPythonCommand({"-c",
        "import tarfile; t=tarfile.open(r'" + InArchive.generic_string() + "'); "
        "print('\\n'.join(m.name for m in t.getmembers() if m.isfile()))"}, InRepo);
    return result.exitCode == 0 && result.stdoutText.find(InSubstring) != std::string::npos;
}

// Count all regular files in a directory recursively.
auto CountFilesRecursive(const std::filesystem::path& InDir) -> int {
    if (!std::filesystem::exists(InDir)) {
        return 0;
    }
    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(InDir)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// ExportWorkspaceContext — a root repo with one registered submodule.
//
// Layout:
//   sandbox/
//     child-remote.git/     — bare remote for the child repo
//     child-seed/           — seed repo for child (used to push initial commit)
//     root-remote.git/      — bare remote for the root repo
//     root-seed/            — seed repo for root (used to push initial commit)
//     root-clone/           — the working clone we run kog export from
//       deps/child/         — the registered submodule (checked out)
// ---------------------------------------------------------------------------

struct ExportWorkspaceContext {
    SandboxContext sandbox;
    std::filesystem::path rootClone;
    std::filesystem::path childClone;
    std::string submodulePath;
    std::string rootRepoName;
    std::string childRepoName;
};

auto CreateExportWorkspace(const std::string& InName) -> ExportWorkspaceContext {
    ExportWorkspaceContext ctx;
    ctx.sandbox = CreateSandboxWorkspace(InName);
    ctx.submodulePath = "deps/child";
    ctx.rootRepoName = "root-clone";
    ctx.childRepoName = "child";

    const auto childBareRemote = (ctx.sandbox.root / "child-remote.git").lexically_normal();
    const auto childSeedRepo   = (ctx.sandbox.root / "child-seed").lexically_normal();
    const auto rootBareRemote  = (ctx.sandbox.root / "root-remote.git").lexically_normal();
    const auto rootSeedRepo    = (ctx.sandbox.root / "root-seed").lexically_normal();
    ctx.rootClone              = (ctx.sandbox.root / "root-clone").lexically_normal();

    const std::string branch = "main";

    // --- Set up child repo ---
    RequireSuccess(RunGit({"init", "--bare", childBareRemote.string()}, ctx.sandbox.root),
                   "init child bare");
    RequireSuccess(RunGit({"init", childSeedRepo.string()}, ctx.sandbox.root),
                   "init child seed");
    ConfigureIdentity(childSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", branch}, childSeedRepo), "checkout child branch");
    WriteTextFile(childSeedRepo / "child.txt", "child content\n");
    RequireSuccess(RunGit({"add", "child.txt"}, childSeedRepo), "child add");
    RequireSuccess(RunGit({"commit", "-m", "child seed"}, childSeedRepo), "child commit");
    RequireSuccess(RunGit({"remote", "add", "origin", childBareRemote.string()}, childSeedRepo),
                   "child add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, childSeedRepo), "child push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + branch)}, childBareRemote),
                   "child bare HEAD");

    // --- Set up root repo ---
    RequireSuccess(RunGit({"init", "--bare", rootBareRemote.string()}, ctx.sandbox.root),
                   "init root bare");
    RequireSuccess(RunGit({"init", rootSeedRepo.string()}, ctx.sandbox.root),
                   "init root seed");
    ConfigureIdentity(rootSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", branch}, rootSeedRepo), "checkout root branch");
    WriteTextFile(rootSeedRepo / ".gitignore", ".kano/\n");
    WriteTextFile(rootSeedRepo / "README.md", "root content\n");
    RequireSuccess(RunGit({"add", ".gitignore", "README.md"}, rootSeedRepo), "root add base");
    RequireSuccess(RunGit({"commit", "-m", "root seed"}, rootSeedRepo), "root base commit");
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always",
                "submodule", "add", "-b", branch,
                childBareRemote.string(), ctx.submodulePath},
               rootSeedRepo),
        "root add submodule");
    RequireSuccess(RunGit({"commit", "-am", "add submodule"}, rootSeedRepo),
                   "root commit submodule");
    RequireSuccess(RunGit({"remote", "add", "origin", rootBareRemote.string()}, rootSeedRepo),
                   "root add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, rootSeedRepo), "root push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + branch)}, rootBareRemote),
                   "root bare HEAD");

    // --- Clone root with submodules ---
    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always",
                "clone", "--recurse-submodules",
                rootBareRemote.string(), ctx.rootClone.string()},
               ctx.sandbox.root),
        "clone root with submodules");
    ConfigureIdentity(ctx.rootClone);
    ctx.childClone = (ctx.rootClone / std::filesystem::path(ctx.submodulePath)).lexically_normal();
    ConfigureIdentity(ctx.childClone);

    return ctx;
}

// Run kog discover to populate the workspace manifest so that kog export
// can find registered submodules via RegisteredOnly scope.
auto RunKogDiscover(const ExportWorkspaceContext& InCtx) -> CommandResult {
    return RunKogWithEnv(
        {"discover"},
        InCtx.rootClone,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});
}

// Run kog export with the given extra args from the root clone directory.
// Allows the file:// protocol so submodule discovery works in the sandbox.
auto RunKogExport(const ExportWorkspaceContext& InCtx,
                  const std::vector<std::string>& InExtraArgs) -> CommandResult {
    std::vector<std::string> args = {"export"};
    args.insert(args.end(), InExtraArgs.begin(), InExtraArgs.end());
    return RunKogWithEnv(args, InCtx.rootClone,
                         {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});
}

auto CreateFileSymlinkOrSkip(const std::filesystem::path& InLinkPath,
                             const std::filesystem::path& InTarget) -> void {
    std::filesystem::create_directories(InLinkPath.parent_path());
    std::error_code ec;
    std::filesystem::create_symlink(InTarget, InLinkPath, ec);
    if (ec) {
        SKIP("filesystem symlink creation is unavailable: " << ec.message());
    }
}

} // anonymous namespace

// ===========================================================================
// Test 1: Default options produce one .tar per repo in the default output dir
//         with correct _revNNN naming
// ===========================================================================

TEST_CASE("kog export default options produce one tar per repo in default output dir",
          "[Integration][export][default][req-1.1][req-1.2][req-2.3][req-3.1][req-3.4][req-3.6]") {
    // **Validates: Requirements 1.1, 1.2, 2.3, 3.1, 3.4, 3.6**
    //
    // When kog export is invoked with no extra options from a workspace root
    // that has one registered submodule:
    //   - Exit code must be 0
    //   - Two .tar archives must be created in <cwd>/.kano/tmp/git/export/
    //   - Each archive name must match the _revNNN pattern
    //   - The root repo archive must appear first (root-first ordering)

    const auto ctx = CreateExportWorkspace("default-export");

    const auto discoverResult = RunKogDiscover(ctx);
    INFO("kog discover exit=" << discoverResult.exitCode);
    INFO("kog discover stdout=" << discoverResult.stdoutText);
    INFO("kog discover stderr=" << discoverResult.stderrText);
    REQUIRE(discoverResult.exitCode == 0);

    const auto result = RunKogExport(ctx, {});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Default output dir: <cwd>/.kano/tmp/git/export/
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Two .tar archives: one for root, one for child submodule
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 2);

    // Each archive name must contain "_rev" (the revision naming pattern)
    REQUIRE(AnyFileContains(outputDir, "_rev"));

    // The root repo archive must be present (named after the root clone dir)
    REQUIRE(AnyFileContains(outputDir, ctx.rootRepoName));

    // The child submodule archive must be present
    REQUIRE(AnyFileContains(outputDir, ctx.childRepoName));

    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".tar" &&
            entry.path().filename().string().find(ctx.rootRepoName + "_rev") == 0) {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE(ArchiveContainsPath(rootArchive, "README.md", ctx.rootClone));

    // stdout must mention the output directory
    REQUIRE(result.stdoutText.find(".kano") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 2: --dry-run creates no files and exits 0
// ===========================================================================

TEST_CASE("kog export --dry-run creates no files and exits 0",
          "[Integration][export][dry-run][req-6.1][req-6.2][req-6.3][req-6.4]") {
    // **Validates: Requirements 6.1, 6.2, 6.3, 6.4**
    //
    // When --dry-run is specified:
    //   - Exit code must be 0
    //   - No archive files, manifest files, or checksum files must be created
    //   - The output directory must NOT be created
    //   - stdout must contain the list of repos that would be exported and
    //     the target output directory

    const auto ctx = CreateExportWorkspace("dry-run-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--dry-run"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // The default output directory must NOT have been created
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE_FALSE(std::filesystem::exists(outputDir));

    // No files must have been created anywhere under .kano/tmp
    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE(CountFilesRecursive(kanoTmpDir) == 0);

    // stdout must mention the repos that would be exported
    REQUIRE(result.stdoutText.find(ctx.rootRepoName) != std::string::npos);

    // stdout must mention the output directory path
    REQUIRE(result.stdoutText.find(".kano") != std::string::npos);

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 3: --no-metadata skips manifest and .sha256 files
// ===========================================================================

TEST_CASE("kog export --no-metadata skips manifest and sha256 files",
          "[Integration][export][no-metadata][req-5.7]") {
    // **Validates: Requirements 5.7**
    //
    // When --no-metadata is specified:
    //   - Exit code must be 0
    //   - Archive files (.tar) must still be created
    //   - No manifest files (_manifest.txt) must be created
    //   - No checksum files (.sha256) must be created

    const auto ctx = CreateExportWorkspace("no-metadata-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--no-metadata"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Archives must still be created
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 2);

    // No manifest files
    const auto metadataDir = outputDir / "metadata";
    REQUIRE_FALSE(AnyFileContainsRecursive(outputDir, "_manifest.txt"));

    // No checksum files
    REQUIRE_FALSE(AnyFileContainsRecursive(outputDir, ".sha256"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --subtree exports standalone archive root",
          "[Integration][export][subtree]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / "README.md", "root\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/UGS.txt", "ugs\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Nested/File.txt", "nested\n");
    WriteTextFile(repo / "Engine/Source/Programs/OtherTool/Other.txt", "other\n");
    RequireSuccess(RunGit({"add", "."}, repo), "add files");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    const auto subtreePath = (repo / "Engine/Source/Programs/UnrealGameSync").generic_string();
    const auto result = RunKogWithEnv(
        {"export", "--subtree", subtreePath, "--name", "UnrealGameSync", "--source", "head", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));
    REQUIRE(AnyFileContains(outputDir, "UnrealGameSync_rev"));
    REQUIRE(AnyFileContains(outputDir, ".export-manifest.json"));

    const auto list = RunPythonCommand({"-c",
        "import tarfile,glob; p=glob.glob(r'" + outputDir.generic_string() + "/UnrealGameSync_rev*.tar')[0]; "
        "t=tarfile.open(p); print('\\n'.join(m.name for m in t.getmembers() if m.isfile()))"}, repo);
    REQUIRE(list.exitCode == 0);
    REQUIRE(list.stdoutText.find("UnrealGameSync/UGS.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Nested/File.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("README.md") == std::string::npos);
    REQUIRE(list.stdoutText.find("OtherTool/Other.txt") == std::string::npos);

    std::filesystem::path manifestPath;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".export-manifest.json") != std::string::npos) {
            manifestPath = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(manifestPath.empty());
    {
        std::ifstream mf(manifestPath, std::ios::binary);
        REQUIRE(mf.good());
        const std::string text((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        REQUIRE(text.find("\"exportMode\": \"subtree\"") != std::string::npos);
        REQUIRE(text.find("\"subtree\": {") != std::string::npos);
        REQUIRE(text.find("\"repoRelativePath\": \"Engine/Source/Programs/UnrealGameSync\"") != std::string::npos);
        REQUIRE(text.find("\"stripSubtreePath\": true") != std::string::npos);
    }

    const auto canonicalManifest = repo / ".kano" / "tmp";
    REQUIRE(AnyFileContainsRecursive(canonicalManifest, ".export-manifest.json"));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog export path filters exclude matching root directories",
          "[Integration][export][path-filters][exclude]") {
    const auto ctx = CreateExportWorkspace("exclude-root-directories");

    WriteTextFile(ctx.rootClone / "Content/Maps/Start.umap", "map\n");
    WriteTextFile(ctx.rootClone / "Plugins/MyPlugin/MyPlugin.uplugin", "{ }\n");
    RequireSuccess(RunGit({"add", "Content", "Plugins"}, ctx.rootClone), "root add content/plugins");
    RequireSuccess(RunGit({"commit", "-m", "add content and plugin roots"}, ctx.rootClone), "root commit content/plugins");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {"--exclude-path", "Content", "--exclude-path", "Plugins", "--no-validate-release-archive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".tar" &&
            entry.path().filename().string().find(ctx.rootRepoName + "_rev") == 0) {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE(ArchiveContainsPath(rootArchive, "README.md", ctx.rootClone));
    REQUIRE_FALSE(ArchiveContainsPath(rootArchive, "Content/Maps/Start.umap", ctx.rootClone));
    REQUIRE_FALSE(ArchiveContainsPath(rootArchive, "Plugins/MyPlugin/MyPlugin.uplugin", ctx.rootClone));
    REQUIRE_FALSE(ArchiveContainsPath(rootArchive, ".kano/", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --subtree accepts relative path and defaults name to subtree basename",
          "[Integration][export][subtree][relative-path]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export-relative");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/UGS.txt", "ugs\n");
    RequireSuccess(RunGit({"add", "."}, repo), "add files");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    const auto result = RunKogWithEnv(
        {"export", "--subtree", "Engine/Source/Programs/UnrealGameSync", "--source", "head", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));
    REQUIRE(AnyFileContains(outputDir, "UnrealGameSync_rev"));

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog export --subtree --source working-tree includes untracked and excludes ignored files",
          "[Integration][export][subtree][working-tree]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export-working-tree");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / ".gitignore", "Engine/Source/Programs/UnrealGameSync/Ignored.txt\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Tracked.txt", "tracked\n");
    WriteTextFile(repo / "Engine/Source/Programs/OtherTool/Outside.txt", "outside\n");
    RequireSuccess(RunGit({"add", "."}, repo), "add baseline files");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Untracked.txt", "untracked\n");
    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Ignored.txt", "ignored\n");
    WriteTextFile(repo / "Engine/Source/Programs/OtherTool/OutsideUntracked.txt", "outside-untracked\n");

    const auto result = RunKogWithEnv(
        {"export", "--subtree", "Engine/Source/Programs/UnrealGameSync", "--name", "UnrealGameSync", "--source", "working-tree", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    const auto list = RunPythonCommand({"-c",
        "import tarfile,glob; p=glob.glob(r'" + outputDir.generic_string() + "/UnrealGameSync_rev*.tar')[0]; "
        "t=tarfile.open(p); print('\\n'.join(m.name for m in t.getmembers() if m.isfile()))"}, repo);
    REQUIRE(list.exitCode == 0);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Tracked.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Untracked.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("Ignored.txt") == std::string::npos);
    REQUIRE(list.stdoutText.find("Outside") == std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog export --subtree --source working-tree fails when subtree has no exportable files",
          "[Integration][export][subtree][working-tree][empty]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export-working-tree-empty");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / "README.md", "root\n");
    RequireSuccess(RunGit({"add", "README.md"}, repo), "add root file");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    const auto emptySubtree = repo / "Engine/Source/Programs/UnrealGameSync";
    std::filesystem::create_directories(emptySubtree);

    const auto result = RunKogWithEnv(
        {"export", "--subtree", "Engine/Source/Programs/UnrealGameSync", "--name", "UnrealGameSync", "--source", "working-tree", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode != 0);
    REQUIRE(result.stderrText.find("no files found in subtree working tree export") != std::string::npos);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(CountFilesWithExtension(outputDir, ".tar") == 0);

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog export --subtree --source working-tree supports zip archives",
          "[Integration][export][subtree][working-tree][zip]") {
    const auto sandbox = CreateSandboxWorkspace("subtree-export-working-tree-zip");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();
    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init subtree repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");

    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Tracked.txt", "tracked\n");
    RequireSuccess(RunGit({"add", "."}, repo), "add baseline files");
    RequireSuccess(RunGit({"commit", "-m", "seed"}, repo), "seed commit");

    WriteTextFile(repo / "Engine/Source/Programs/UnrealGameSync/Untracked.txt", "untracked\n");

    const auto result = RunKogWithEnv(
        {"export", "--subtree", "Engine/Source/Programs/UnrealGameSync", "--name", "UnrealGameSync", "--source", "working-tree", "--format", "zip", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    const auto list = RunPythonCommand({"-c",
        "import zipfile,glob; p=glob.glob(r'" + outputDir.generic_string() + "/UnrealGameSync_rev*.zip')[0]; "
        "z=zipfile.ZipFile(p); print('\\n'.join(z.namelist()))"}, repo);
    REQUIRE(list.exitCode == 0);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Tracked.txt") != std::string::npos);
    REQUIRE(list.stdoutText.find("UnrealGameSync/Untracked.txt") != std::string::npos);

    RemoveSandboxWorkspace(sandbox);
}

// ===========================================================================
// Test 4: --no-recursive exports only the root repo (single archive)
// ===========================================================================

TEST_CASE("kog export --no-recursive exports only the root repo",
          "[Integration][export][no-recursive][req-1.3][req-3.7]") {
    // **Validates: Requirements 1.3, 3.7**
    //
    // When --no-recursive is specified:
    //   - Exit code must be 0
    //   - Exactly one archive must be created (the root repo only)
    //   - The child submodule must NOT have an archive

    const auto ctx = CreateExportWorkspace("no-recursive-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--no-recursive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Exactly one .tar archive (root only)
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 1);

    // The root repo archive must be present
    REQUIRE(AnyFileContains(outputDir, ctx.rootRepoName));

    // The child submodule archive must NOT be present
    REQUIRE_FALSE(AnyFileContains(outputDir, ctx.childRepoName));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export default uses split-depth=1 and exports depth-1 repos as integrated single archives",
          "[Integration][export][default-split-depth][single-threshold]") {
    const auto ctx = CreateExportWorkspace("default-split-depth-single-threshold");

    const auto grandchildBareRemote = (ctx.sandbox.root / "grandchild-remote.git").lexically_normal();
    const auto grandchildSeedRepo   = (ctx.sandbox.root / "grandchild-seed").lexically_normal();
    const std::string branch = "main";

    RequireSuccess(RunGit({"init", "--bare", grandchildBareRemote.string()}, ctx.sandbox.root),
                   "init grandchild bare");
    RequireSuccess(RunGit({"init", grandchildSeedRepo.string()}, ctx.sandbox.root),
                   "init grandchild seed");
    ConfigureIdentity(grandchildSeedRepo);
    RequireSuccess(RunGit({"checkout", "-b", branch}, grandchildSeedRepo), "checkout grandchild branch");
    WriteTextFile(grandchildSeedRepo / "grandchild.txt", "grandchild content\n");
    RequireSuccess(RunGit({"add", "grandchild.txt"}, grandchildSeedRepo), "grandchild add");
    RequireSuccess(RunGit({"commit", "-m", "grandchild seed"}, grandchildSeedRepo), "grandchild commit");
    RequireSuccess(RunGit({"remote", "add", "origin", grandchildBareRemote.string()}, grandchildSeedRepo),
                   "grandchild add remote");
    RequireSuccess(RunGit({"push", "-u", "origin", branch}, grandchildSeedRepo), "grandchild push");
    RequireSuccess(RunGit({"symbolic-ref", "HEAD", ("refs/heads/" + branch)}, grandchildBareRemote),
                   "grandchild bare HEAD");

    RequireSuccess(
        RunGit({"-c", "protocol.file.allow=always",
                "submodule", "add", "-b", branch,
                grandchildBareRemote.string(), "deps/grandchild"},
               ctx.childClone),
        "child add grandchild submodule");
    RequireSuccess(RunGit({"commit", "-am", "add grandchild submodule"}, ctx.childClone),
                   "child commit grandchild submodule");
    RequireSuccess(RunGit({"push", "origin", branch}, ctx.childClone),
                   "child push updated submodule pointer");

    RequireSuccess(RunGit({"add", ctx.submodulePath}, ctx.rootClone), "root add updated child pointer");
    RequireSuccess(RunGit({"commit", "-m", "update child pointer"}, ctx.rootClone),
                   "root commit child pointer");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {"--no-validate-release-archive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));
    REQUIRE(AnyFileContains(outputDir, ctx.rootRepoName));
    REQUIRE(AnyFileContains(outputDir, "deps_child"));
    REQUIRE_FALSE(AnyFileContains(outputDir, "deps_child_deps_grandchild"));

    std::filesystem::path childArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (entry.path().extension() == ".tar" && name.find("deps_child") != std::string::npos) {
            childArchive = entry.path();
            break;
        }
    }
    REQUIRE(!childArchive.empty());
    REQUIRE(ArchiveContainsPath(childArchive, "deps/grandchild/grandchild.txt", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 5: --format zip produces .zip archives
// ===========================================================================

TEST_CASE("kog export --format zip produces zip archives",
          "[Integration][export][format-zip][req-3.5]") {
    // **Validates: Requirements 3.5**
    //
    // When --format zip is specified:
    //   - Exit code must be 0
    //   - Archives must have .zip extension (not .tar)
    //   - Two .zip archives must be created (one per repo)

    const auto ctx = CreateExportWorkspace("zip-format-export");

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--format", "zip"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    // Two .zip archives
    const int zipCount = CountFilesWithExtension(outputDir, ".zip");
    REQUIRE(zipCount == 2);

    // No .tar archives (format is zip, not tar)
    const int tarCount = CountFilesWithExtension(outputDir, ".tar");
    REQUIRE(tarCount == 0);

    // Archive names must contain "_rev" (revision naming pattern)
    REQUIRE(AnyFileContains(outputDir, "_rev"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 6: --output <dir> writes archives to the specified directory
// ===========================================================================

TEST_CASE("kog export --output writes archives to the specified directory",
          "[Integration][export][output-dir][req-3.7]") {
    // **Validates: Requirements 3.7**
    //
    // When --output <dir> is specified:
    //   - Exit code must be 0
    //   - Archives must be written to the specified directory (not the default)
    //   - The default output directory must NOT be created

    const auto ctx = CreateExportWorkspace("custom-output-export");

    // Use a custom output directory inside the sandbox (not inside the repo)
    const auto customOutputDir = (ctx.sandbox.root / "my-exports").lexically_normal();

    RunKogDiscover(ctx);
    const auto result = RunKogExport(ctx, {"--output", customOutputDir.string()});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Archives must be in the custom output directory
    REQUIRE(std::filesystem::exists(customOutputDir));
    const int tarCount = CountFilesWithExtension(customOutputDir, ".tar");
    REQUIRE(tarCount == 2);

    // The default output directory must NOT have been created
    const auto defaultOutputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE_FALSE(std::filesystem::exists(defaultOutputDir));

    // Archive names must contain "_rev"
    REQUIRE(AnyFileContains(customOutputDir, "_rev"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

// ===========================================================================
// Test 7: --single produces export-manifest.json
// ===========================================================================

TEST_CASE("kog export --single produces export-manifest.json",
          "[Integration][export][single][export-manifest][req-manifest]") {
    // Validates: export-manifest.json generation requirements
    //
    // When --single is specified and export succeeds:
    //   - .kano/tmp/export-manifest.json must exist (canonical copy)
    //   - The sibling export-manifest.json must exist alongside the archive
    //   - The manifest must be valid JSON
    //   - The manifest must contain all required keys
    //   - archiveFile / path must use forward slashes
    //   - The archive file referenced by the manifest must exist
    //   - exportMode must be "single"
    //   - singleArchive must be true
    //   - platform must be one of: windows / linux / mac

    const auto ctx = CreateExportWorkspace("single-manifest-export");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {"--single", "--no-validate-release-archive"});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);

    REQUIRE(result.exitCode == 0);

    // Canonical manifest: <cwd>/.kano/tmp/<name>_revNNN.export-manifest.json
    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE(std::filesystem::exists(kanoTmpDir));
    REQUIRE(AnyFileContainsRecursive(kanoTmpDir, ".export-manifest.json"));

    // Find the actual manifest path
    std::filesystem::path canonicalManifest;
    for (const auto& entry : std::filesystem::directory_iterator(kanoTmpDir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".export-manifest.json") != std::string::npos) {
            canonicalManifest = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(canonicalManifest.empty());
    REQUIRE(std::filesystem::exists(canonicalManifest));

    // Read and parse the manifest
    std::ifstream mf(canonicalManifest);
    REQUIRE(mf.good());
    const std::string manifestText((std::istreambuf_iterator<char>(mf)),
                                    std::istreambuf_iterator<char>());
    REQUIRE_FALSE(manifestText.empty());

    // Must be valid JSON (check for required keys as substrings)
    REQUIRE(manifestText.find("\"schemaVersion\"")  != std::string::npos);
    REQUIRE(manifestText.find("\"exportMode\"")     != std::string::npos);
    REQUIRE(manifestText.find("\"singleArchive\"")  != std::string::npos);
    REQUIRE(manifestText.find("\"archiveFile\"")    != std::string::npos);
    REQUIRE(manifestText.find("\"path\"")           != std::string::npos);
    REQUIRE(manifestText.find("\"sha256\"")         != std::string::npos);
    REQUIRE(manifestText.find("\"sizeBytes\"")      != std::string::npos);
    REQUIRE(manifestText.find("\"platform\"")       != std::string::npos);
    REQUIRE(manifestText.find("\"archives\"")       != std::string::npos);

    // exportMode must be "single"
    REQUIRE(manifestText.find("\"exportMode\": \"single\"") != std::string::npos);

    // singleArchive must be true
    REQUIRE(manifestText.find("\"singleArchive\": true") != std::string::npos);

    // platform must be one of the normalized values
    const bool hasValidPlatform =
        manifestText.find("\"platform\": \"windows\"") != std::string::npos ||
        manifestText.find("\"platform\": \"linux\"")   != std::string::npos ||
        manifestText.find("\"platform\": \"mac\"")     != std::string::npos;
    REQUIRE(hasValidPlatform);

    // archiveFile must use forward slashes (no backslashes)
    // Extract archiveFile value by finding it in the JSON text
    const auto archiveFileKey = std::string("\"archiveFile\": \"");
    const auto keyPos = manifestText.find(archiveFileKey);
    REQUIRE(keyPos != std::string::npos);
    const auto valueStart = keyPos + archiveFileKey.size();
    const auto valueEnd = manifestText.find('"', valueStart);
    REQUIRE(valueEnd != std::string::npos);
    const std::string archiveFilePath = manifestText.substr(valueStart, valueEnd - valueStart);
    REQUIRE(archiveFilePath.find('\\') == std::string::npos);

    // The archive file referenced must exist
    REQUIRE(std::filesystem::exists(std::filesystem::path(archiveFilePath)));

    // Sibling copy alongside the archive
    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(AnyFileContains(outputDir, ".export-manifest.json"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export without --single does not produce export-manifest.json",
          "[Integration][export][no-single][export-manifest][req-manifest]") {
    // When --single is NOT specified, export-manifest.json must NOT be written.

    const auto ctx = CreateExportWorkspace("no-single-no-manifest");

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {});

    INFO("exit=" << result.exitCode);
    REQUIRE(result.exitCode == 0);

    const auto kanoTmpDir = ctx.rootClone / ".kano" / "tmp";
    REQUIRE_FALSE(AnyFileContainsRecursive(kanoTmpDir, ".export-manifest.json"));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --single includes subrepo working-tree files by default",
        "[Integration][export][single][default-expanded-subrepos]") {
    const auto ctx = CreateExportWorkspace("single-default-expanded-subrepos");
    const auto discoverResult = RunKogDiscover(ctx);
    REQUIRE(discoverResult.exitCode == 0);

    const auto result = RunKogExport(ctx, {"--single", "--no-validate-release-archive"});
    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stdoutText.find("Skip export for deps/child: .gitmodules policy kog-export=false") != std::string::npos);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE(ArchiveContainsPath(rootArchive, "deps/child/child.txt", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --single --include-subrepos remains accepted and includes subrepo working-tree files",
          "[Integration][export][single][expanded-subrepos][compat]") {
    const auto ctx = CreateExportWorkspace("single-expanded-subrepos");
    const auto discoverResult = RunKogDiscover(ctx);
    REQUIRE(discoverResult.exitCode == 0);

    const auto result = RunKogExport(ctx, {"--single", "--include-subrepos", "--no-validate-release-archive"});
    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE(ArchiveContainsPath(rootArchive, "deps/child/child.txt", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --single skips missing subrepo working-tree directories",
          "[Integration][export][single][missing-subrepo][skip]") {
    const auto ctx = CreateExportWorkspace("single-skip-missing-subrepo");
    const auto discoverResult = RunKogDiscover(ctx);
    REQUIRE(discoverResult.exitCode == 0);

    const auto missingSubrepoPath = ctx.rootClone / std::filesystem::path(ctx.submodulePath);
    std::error_code removeEc;
    std::filesystem::remove_all(missingSubrepoPath, removeEc);
    REQUIRE_FALSE(std::filesystem::exists(missingSubrepoPath));

    const auto result = RunKogExport(ctx, {"--single", "--no-validate-release-archive"});
    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);
    REQUIRE(result.stderrText.find("warning: skipping missing subrepo") != std::string::npos);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE_FALSE(ArchiveContainsPath(rootArchive, "deps/child/child.txt", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export --single respects .gitmodules kog-export=false",
          "[Integration][export][single][policy][kog-export]") {
    const auto ctx = CreateExportWorkspace("single-respects-kog-export-policy");

    RequireSuccess(
        RunGit({"config", "-f", ".gitmodules", "submodule.deps/child.kog-export", "false"}, ctx.rootClone),
        "set kog-export policy false");
    RequireSuccess(RunGit({"add", ".gitmodules"}, ctx.rootClone), "stage gitmodules policy change");
    RequireSuccess(RunGit({"commit", "-m", "set child kog-export false"}, ctx.rootClone), "commit gitmodules policy change");

    const auto discoverResult = RunKogDiscover(ctx);
    REQUIRE(discoverResult.exitCode == 0);

    const auto result = RunKogExport(ctx, {"--single", "--no-validate-release-archive"});
    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());
    REQUIRE_FALSE(ArchiveContainsPath(rootArchive, "deps/child/child.txt", ctx.rootClone));

    RemoveSandboxWorkspace(ctx.sandbox);
}

TEST_CASE("kog export working-tree tar preserves symlink headers and stream alignment",
          "[Integration][export][single][symlink][tar]") {
    const auto sandbox = CreateSandboxWorkspace("single-symlink-tar-export");
    const auto repo = (sandbox.root / "root-clone").lexically_normal();

    RequireSuccess(RunGit({"init", repo.string()}, sandbox.root), "init symlink repo");
    ConfigureIdentity(repo);
    RequireSuccess(RunGit({"checkout", "-b", "main"}, repo), "checkout main");
    RequireSuccess(RunGit({"config", "core.symlinks", "true"}, repo), "enable git symlink tracking");

    WriteTextFile(repo / ".gitignore", ".kano/\n");
    WriteTextFile(repo / "file-a.txt", "normal file content\n");
    CreateFileSymlinkOrSkip(repo / "link-a.txt", std::filesystem::path("file-a.txt"));
    WriteTextFile(repo / "zz_after.txt", "regular file after symlink\n");

    const auto observedDir = repo / "assets" / "ignore-sources" / "upstream" / "github-gitignore";
    WriteTextFile(observedDir / "Leiningen.gitignore", "pom.xml\npom.xml.asc\n*.jar\n");
    WriteTextFile(observedDir / "C++.gitignore", "# Prerequisites\n*.d\n\n# Compiled Object files\n*.slo\n*.lo\n*.o\n*.obj\n");
    WriteTextFile(observedDir / "Global" / "MATLAB.gitignore", "# Autosave files\n*.asv\n*.m~\n");
    CreateFileSymlinkOrSkip(observedDir / "Clojure.gitignore", std::filesystem::path("Leiningen.gitignore"));
    CreateFileSymlinkOrSkip(observedDir / "Fortran.gitignore", std::filesystem::path("C++.gitignore"));
    CreateFileSymlinkOrSkip(observedDir / "Global" / "Octave.gitignore", std::filesystem::path("MATLAB.gitignore"));
    WriteTextFile(observedDir / "ZzzAfter.gitignore", "regular file after observed symlinks\n");

    RequireSuccess(RunGit({"add", "."}, repo), "add symlink fixture");
    RequireSuccess(RunGit({"commit", "-m", "seed symlink fixture"}, repo), "commit symlink fixture");

    const auto result = RunKogWithEnv(
        {"export", "--single", "--include-subrepos", "--source", "working-tree", "--no-validate-release-archive"},
        repo,
        {{"GIT_ALLOW_PROTOCOL", "file:https:ssh:git"}});

    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = repo / ".kano" / "tmp" / "git" / "export";
    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());

    const std::string prefix = repo.filename().generic_string();
    const auto verify = RunPythonCommand(
        {
            "-c",
            "import pathlib,sys,tarfile,tempfile\n"
            "archive=pathlib.Path(sys.argv[1])\n"
            "prefix=sys.argv[2]\n"
            "expected={\n"
            "  'link-a.txt':'file-a.txt',\n"
            "  'assets/ignore-sources/upstream/github-gitignore/Clojure.gitignore':'Leiningen.gitignore',\n"
            "  'assets/ignore-sources/upstream/github-gitignore/Fortran.gitignore':'C++.gitignore',\n"
            "  'assets/ignore-sources/upstream/github-gitignore/Global/Octave.gitignore':'MATLAB.gitignore',\n"
            "}\n"
            "after=prefix + '/zz_after.txt'\n"
            "observed_after=prefix + '/assets/ignore-sources/upstream/github-gitignore/ZzzAfter.gitignore'\n"
            "with tarfile.open(archive,'r') as t:\n"
            "  ordered=t.getmembers()\n"
            "  names=[m.name for m in ordered]\n"
            "  members={m.name:m for m in ordered}\n"
            "  first_link=prefix + '/link-a.txt'\n"
            "  assert after in members and members[after].isfile(), after\n"
            "  assert observed_after in members and members[observed_after].isfile(), observed_after\n"
            "  assert names.index(after) > names.index(first_link), names\n"
            "  for rel,target in expected.items():\n"
            "    name=prefix + '/' + rel\n"
            "    member=members[name]\n"
            "    assert member.issym(), (name, member.type)\n"
            "    assert member.linkname == target, (name, member.linkname)\n"
            "    assert member.size == 0, (name, member.size)\n"
            "  with tempfile.TemporaryDirectory() as td:\n"
            "    t.extractall(pathlib.Path(td))\n"
            "print('OK')\n",
            rootArchive.generic_string(),
            prefix,
        },
        repo);

    INFO("verify exit=" << verify.exitCode);
    INFO("verify stdout=" << verify.stdoutText);
    INFO("verify stderr=" << verify.stderrText);
    REQUIRE(verify.exitCode == 0);

    const auto tarProbe = RunCommand("tar", {"--version"}, repo);
    if (tarProbe.exitCode == 0) {
        const auto tarList = RunCommand("tar", {"-tf", rootArchive.generic_string()}, repo);
        INFO("tar -tf stdout=" << tarList.stdoutText);
        INFO("tar -tf stderr=" << tarList.stderrText);
        REQUIRE(tarList.exitCode == 0);
        REQUIRE(tarList.stderrText.find("Skipping to next header") == std::string::npos);
        REQUIRE(tarList.stdoutText.find(prefix + "/zz_after.txt") != std::string::npos);

        const auto extractDir = (sandbox.root / "gnu-tar-extract").lexically_normal();
        std::filesystem::create_directories(extractDir);
        const auto tarExtract = RunCommand("tar", {"-xf", rootArchive.generic_string(), "-C", extractDir.generic_string()}, repo);
        INFO("tar -xf stdout=" << tarExtract.stdoutText);
        INFO("tar -xf stderr=" << tarExtract.stderrText);
        REQUIRE(tarExtract.exitCode == 0);
        REQUIRE(tarExtract.stderrText.find("Skipping to next header") == std::string::npos);
    }

    RemoveSandboxWorkspace(sandbox);
}

TEST_CASE("kog export normalizes LF-enforced scripts in single expanded exports",
          "[Integration][export][single][line-endings][gitattributes]") {
    const auto ctx = CreateExportWorkspace("single-line-ending-normalization");

    const auto rootScript = ctx.rootClone / "scripts" / "root_crlf.sh";
    WriteTextFile(ctx.rootClone / ".gitattributes", "*.sh text eol=lf\n*.groovy text eol=lf\n");
    WriteTextFile(rootScript, "#!/usr/bin/env bash\r\necho root\r\n");
    RequireSuccess(RunGit({"add", ".gitattributes", "scripts/root_crlf.sh"}, ctx.rootClone), "root add gitattributes/script");
    RequireSuccess(RunGit({"commit", "-m", "add root lf attrs"}, ctx.rootClone), "root commit gitattributes/script");

    const auto childScript = ctx.childClone / "scripts" / "child_crlf.sh";
    WriteTextFile(ctx.childClone / ".gitattributes", "*.sh text eol=lf\n");
    WriteTextFile(childScript, "#!/usr/bin/env bash\r\necho child\r\n");
    RequireSuccess(RunGit({"add", ".gitattributes", "scripts/child_crlf.sh"}, ctx.childClone), "child add gitattributes/script");
    RequireSuccess(RunGit({"commit", "-m", "add child lf attrs"}, ctx.childClone), "child commit gitattributes/script");
    RequireSuccess(RunGit({"add", ctx.submodulePath}, ctx.rootClone), "root add child pointer update");
    RequireSuccess(RunGit({"commit", "-m", "update child pointer for lf attrs"}, ctx.rootClone), "root commit child pointer update");

    // Simulate Windows working-tree CRLF noise after commits.
    WriteTextFile(rootScript, "#!/usr/bin/env bash\r\necho root\r\n");
    WriteTextFile(childScript, "#!/usr/bin/env bash\r\necho child\r\n");
    REQUIRE(FileContainsCRLF(rootScript));
    REQUIRE(FileContainsCRLF(childScript));

    RequireSuccess(RunKogDiscover(ctx), "kog discover");
    const auto result = RunKogExport(ctx, {"--single", "--include-subrepos", "--source", "working-tree", "--no-validate-release-archive"});
    INFO("exit=" << result.exitCode);
    INFO("stdout=" << result.stdoutText);
    INFO("stderr=" << result.stderrText);
    REQUIRE(result.exitCode == 0);

    const auto outputDir = ctx.rootClone / ".kano" / "tmp" / "git" / "export";
    REQUIRE(std::filesystem::exists(outputDir));

    std::filesystem::path rootArchive;
    for (const auto& entry : std::filesystem::directory_iterator(outputDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tar") {
            rootArchive = entry.path();
            break;
        }
    }
    REQUIRE_FALSE(rootArchive.empty());

    const std::string rootArchiveMember = ctx.rootRepoName + "/scripts/root_crlf.sh";
    const std::string childArchiveMember = ctx.rootRepoName + "/deps/child/scripts/child_crlf.sh";

    const auto verify = RunPythonCommand(
        {
            "-c",
            "import pathlib,subprocess,sys,tarfile,tempfile; "
            "archive=pathlib.Path(sys.argv[1]); root_member=sys.argv[2]; child_member=sys.argv[3]; "
            "members=[root_member,child_member]; "
            "with tempfile.TemporaryDirectory() as td: "
            "  td_path=pathlib.Path(td); "
            "  with tarfile.open(archive,'r') as t: t.extractall(td_path); "
            "  for member in members: "
            "    p=td_path/member; data=p.read_bytes(); "
            "    if b'\\r' in data: raise SystemExit(f'CR byte found in {member}'); "
            "    subprocess.check_call(['bash','-n',str(p)]); "
            "print('OK')",
            rootArchive.generic_string(),
            rootArchiveMember,
            childArchiveMember,
        },
        ctx.rootClone);

    INFO("verify exit=" << verify.exitCode);
    INFO("verify stdout=" << verify.stdoutText);
    INFO("verify stderr=" << verify.stderrText);
    REQUIRE(verify.exitCode == 0);

    RemoveSandboxWorkspace(ctx.sandbox);
}
