#include <CLI/CLI.hpp>
#include "shell_executor.hpp"
#include "terminal_color.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace kano::git::commands
{
namespace
{

auto CheckCommand(const std::string& Name, const std::string& Command, const std::vector<std::string>& Args,
                  const std::optional<std::filesystem::path>& WorkingDir = std::nullopt) -> int
{
    const auto result = shell::ExecuteCommand(Command, Args, shell::ExecMode::Capture, WorkingDir);
    if (result.exitCode == 0)
    {
        std::cout << kano::terminal::PassTag() << " " << Name << "\n";
        return 0;
    }
    std::cerr << kano::terminal::FailTag() << " " << Name << "\n";
    if (!result.stderrStr.empty())
    {
        std::cerr << result.stderrStr;
    }
    return 1;
}

auto TrimCopy(std::string Value) -> std::string
{
    auto IsSpace = [](unsigned char Ch) { return std::isspace(Ch) != 0; };
    Value.erase(Value.begin(), std::find_if(Value.begin(), Value.end(), [&](char Ch) { return !IsSpace(static_cast<unsigned char>(Ch)); }));
    Value.erase(std::find_if(Value.rbegin(), Value.rend(), [&](char Ch) { return !IsSpace(static_cast<unsigned char>(Ch)); }).base(), Value.end());
    return Value;
}

auto ToLowerCopy(std::string Value) -> std::string
{
    std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
    return Value;
}

auto StartsWith(const std::string& Text, const std::string& Prefix) -> bool
{
    return Text.rfind(Prefix, 0) == 0;
}

auto ContainsUnsafeOwnershipGitError(const std::string& Text) -> bool
{
    const auto Lower = ToLowerCopy(Text);
    return Lower.find("detected dubious ownership") != std::string::npos || Lower.find("safe.directory") != std::string::npos;
}

auto FirstLine(const std::string& Text) -> std::string
{
    std::istringstream Stream(Text);
    std::string Line;
    std::getline(Stream, Line);
    return TrimCopy(Line);
}

auto NormalizePath(const std::filesystem::path& Path) -> std::filesystem::path
{
    std::error_code Ec;
    auto Absolute = std::filesystem::absolute(Path, Ec);
    if (Ec)
    {
        Absolute = Path;
    }

    auto Canonical = std::filesystem::weakly_canonical(Absolute, Ec);
    if (Ec)
    {
        Canonical = Absolute.lexically_normal();
    }
    return Canonical.lexically_normal();
}

auto NormalizeSafeDirectoryKey(const std::filesystem::path& Path) -> std::string
{
    auto Text = NormalizePath(Path).generic_string();
    while (Text.size() > 3 && Text.back() == '/')
    {
        Text.pop_back();
    }
    return ToLowerCopy(Text);
}

auto NormalizeConfiguredSafeDirectoryKey(std::string Value) -> std::string
{
    Value = TrimCopy(Value);
    std::replace(Value.begin(), Value.end(), '\\', '/');
    while (Value.size() > 3 && Value.back() == '/')
    {
        Value.pop_back();
    }
    return ToLowerCopy(Value);
}

auto IsPathInsideRoot(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
{
    const auto NormalRoot = Root.lexically_normal();
    const auto NormalCandidate = Candidate.lexically_normal();
    const auto Relative = NormalCandidate.lexically_relative(NormalRoot);
    const auto RelativeText = Relative.generic_string();
    return RelativeText == "." || (!RelativeText.empty() && !Relative.is_absolute() && !StartsWith(RelativeText, ".."));
}

auto LooksLikeGitWorktree(const std::filesystem::path& Repo) -> bool
{
    std::error_code Ec;
    return std::filesystem::exists(Repo / ".git", Ec);
}

auto ReadGitmodulesPaths(const std::filesystem::path& Repo) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> Paths;
    std::ifstream Input(Repo / ".gitmodules");
    if (!Input)
    {
        return Paths;
    }

    std::string Line;
    while (std::getline(Input, Line))
    {
        Line = TrimCopy(Line);
        const auto Equals = Line.find('=');
        if (Equals == std::string::npos)
        {
            continue;
        }

        const auto Key = TrimCopy(Line.substr(0, Equals));
        if (Key != "path")
        {
            continue;
        }

        const auto Value = TrimCopy(Line.substr(Equals + 1));
        if (Value.empty())
        {
            continue;
        }

        const auto RelativePath = std::filesystem::path(Value).lexically_normal();
        const auto RelativeText = RelativePath.generic_string();
        if (RelativePath.is_absolute() || RelativeText.empty() || RelativeText == "." || StartsWith(RelativeText, ".."))
        {
            continue;
        }

        Paths.push_back(RelativePath);
    }
    return Paths;
}

auto DiscoverSafeDirectoryCandidates(const std::filesystem::path& Root) -> std::vector<std::filesystem::path>
{
    const auto NormalRoot = NormalizePath(Root);
    std::vector<std::filesystem::path> Candidates;
    std::set<std::string> Seen;

    std::function<void(const std::filesystem::path&)> Visit = [&](const std::filesystem::path& Repo) {
        const auto NormalRepo = NormalizePath(Repo);
        if (!IsPathInsideRoot(NormalRoot, NormalRepo) || !LooksLikeGitWorktree(NormalRepo))
        {
            return;
        }

        const auto Key = NormalizeSafeDirectoryKey(NormalRepo);
        if (!Seen.insert(Key).second)
        {
            return;
        }

        Candidates.push_back(NormalRepo);
        for (const auto& RelativePath : ReadGitmodulesPaths(NormalRepo))
        {
            Visit(NormalRepo / RelativePath);
        }
    };

    Visit(NormalRoot);

    std::error_code Ec;
    for (const auto& Entry : std::filesystem::directory_iterator(NormalRoot, Ec))
    {
        if (!Entry.is_directory(Ec))
        {
            continue;
        }
        Visit(Entry.path());
    }

    return Candidates;
}

struct SafeDirectoryProbe
{
    int exitCode = 0;
    bool unsafeOwnership = false;
    std::string stderrText;
};

auto ProbeSafeDirectory(const std::filesystem::path& Repo) -> SafeDirectoryProbe
{
    const auto Result = shell::ExecuteCommand(
        "git", {"-c", "core.optionalLocks=false", "-C", Repo.string(), "status", "--porcelain"}, shell::ExecMode::Capture);
    return SafeDirectoryProbe{Result.exitCode, Result.exitCode != 0 && ContainsUnsafeOwnershipGitError(Result.stdoutStr + "\n" + Result.stderrStr), Result.stderrStr};
}

auto WorkspaceSafeDirectoryConfigPath(const std::filesystem::path& Root) -> std::filesystem::path
{
    return Root / ".kano" / "git" / "safe-directory.gitconfig";
}

auto ReadSafeDirectoriesFromGitConfigFile(const std::filesystem::path& ConfigPath) -> std::set<std::string>
{
    std::set<std::string> Configured;
    std::ifstream Input(ConfigPath);
    if (!Input)
    {
        return Configured;
    }

    bool InSafeSection = false;
    std::string Line;
    while (std::getline(Input, Line))
    {
        Line = TrimCopy(Line);
        if (Line.empty() || Line.front() == '#' || Line.front() == ';')
        {
            continue;
        }
        if (Line.front() == '[' && Line.back() == ']')
        {
            InSafeSection = ToLowerCopy(Line) == "[safe]";
            continue;
        }
        if (!InSafeSection)
        {
            continue;
        }
        const auto Equals = Line.find('=');
        if (Equals == std::string::npos)
        {
            continue;
        }
        const auto Key = TrimCopy(Line.substr(0, Equals));
        if (Key != "directory")
        {
            continue;
        }
        const auto Value = TrimCopy(Line.substr(Equals + 1));
        if (!Value.empty())
        {
            Configured.insert(NormalizeConfiguredSafeDirectoryKey(Value));
        }
    }
    return Configured;
}

auto ReadConfiguredSafeDirectories(const std::filesystem::path& Root) -> std::set<std::string>
{
    std::set<std::string> Configured;
    const auto Result = shell::ExecuteCommand("git", {"config", "--global", "--get-all", "safe.directory"}, shell::ExecMode::Capture);
    if (Result.exitCode == 0)
    {
        std::istringstream Lines(Result.stdoutStr);
        std::string Line;
        while (std::getline(Lines, Line))
        {
            Line = TrimCopy(Line);
            if (!Line.empty())
            {
                Configured.insert(NormalizeConfiguredSafeDirectoryKey(Line));
            }
        }
    }

    const auto WorkspaceConfigured = ReadSafeDirectoriesFromGitConfigFile(WorkspaceSafeDirectoryConfigPath(Root));
    Configured.insert(WorkspaceConfigured.begin(), WorkspaceConfigured.end());
    return Configured;
}

auto AddGlobalSafeDirectory(const std::filesystem::path& Repo) -> shell::ExecResult
{
    return shell::ExecuteCommand("git", {"config", "--global", "--add", "safe.directory", Repo.generic_string()}, shell::ExecMode::Capture);
}

struct SafeDirectoryAddResult
{
    bool ok = false;
    bool workspaceConfig = false;
    std::string error;
};

auto AppendWorkspaceSafeDirectory(const std::filesystem::path& Root, const std::filesystem::path& Repo) -> SafeDirectoryAddResult
{
    const auto ConfigPath = WorkspaceSafeDirectoryConfigPath(Root);
    std::error_code Ec;
    std::filesystem::create_directories(ConfigPath.parent_path(), Ec);
    if (Ec)
    {
        return SafeDirectoryAddResult{false, true, Ec.message()};
    }

    const bool Exists = std::filesystem::exists(ConfigPath, Ec);
    const auto Size = Exists ? std::filesystem::file_size(ConfigPath, Ec) : 0;
    std::ofstream Output(ConfigPath, std::ios::binary | std::ios::app);
    if (!Output)
    {
        return SafeDirectoryAddResult{false, true, "unable to open workspace safe.directory config"};
    }

    if (!Exists || Size == 0)
    {
        Output << "# Generated by kog doctor --fix-safe-directory\n";
        Output << "[safe]\n";
    }
    else
    {
        Output << "\n[safe]\n";
    }
    Output << "\tdirectory = " << Repo.generic_string() << "\n";
    return SafeDirectoryAddResult{true, true, {}};
}

auto AddSafeDirectoryWithFallback(const std::filesystem::path& Root, const std::filesystem::path& Repo, bool& GlobalConfigUnavailable) -> SafeDirectoryAddResult
{
    if (!GlobalConfigUnavailable)
    {
        const auto GlobalResult = AddGlobalSafeDirectory(Repo);
        if (GlobalResult.exitCode == 0)
        {
            return SafeDirectoryAddResult{true, false, {}};
        }
        GlobalConfigUnavailable = true;
    }

    return AppendWorkspaceSafeDirectory(Root, Repo);
}

auto RunSafeDirectoryDoctor(const std::filesystem::path& Root, bool Fix) -> int
{
    const auto NormalRoot = NormalizePath(Root);
    const auto Candidates = DiscoverSafeDirectoryCandidates(NormalRoot);
    auto ConfiguredSafeDirectories = ReadConfiguredSafeDirectories(NormalRoot);
    const auto WorkspaceConfigPath = WorkspaceSafeDirectoryConfigPath(NormalRoot);
    bool GlobalConfigUnavailable = false;
    int Failures = 0;
    int UnsafeCount = 0;
    int FixedCount = 0;
    int WorkspaceFixedCount = 0;

    std::cout << kano::terminal::InfoTag() << " safe.directory candidates: " << Candidates.size() << "\n";

    for (const auto& Candidate : Candidates)
    {
        const auto Probe = ProbeSafeDirectory(Candidate);
        if (Probe.exitCode == 0)
        {
            continue;
        }

        if (!Probe.unsafeOwnership)
        {
            ++Failures;
            std::cerr << kano::terminal::FailTag() << " cannot inspect repo: " << Candidate.generic_string() << "\n";
            if (!Probe.stderrText.empty())
            {
                std::cerr << FirstLine(Probe.stderrText) << "\n";
            }
            continue;
        }

        ++UnsafeCount;
        std::cerr << kano::terminal::WarnTag() << " unsafe ownership: " << Candidate.generic_string() << "\n";

        if (!Fix)
        {
            continue;
        }

        const auto CandidateKey = NormalizeSafeDirectoryKey(Candidate);
        std::string FixSource = "configured";
        if (ConfiguredSafeDirectories.find(CandidateKey) != ConfiguredSafeDirectories.end())
        {
            std::cout << kano::terminal::InfoTag() << " safe.directory already configured: " << Candidate.generic_string() << "\n";
        }
        else
        {
            const auto AddResult = AddSafeDirectoryWithFallback(NormalRoot, Candidate, GlobalConfigUnavailable);
            if (!AddResult.ok)
            {
                ++Failures;
                std::cerr << kano::terminal::FailTag() << " failed to add safe.directory: " << Candidate.generic_string() << "\n";
                if (!AddResult.error.empty())
                {
                    std::cerr << AddResult.error << "\n";
                }
                continue;
            }
            ConfiguredSafeDirectories.insert(CandidateKey);
            ++FixedCount;
            if (AddResult.workspaceConfig)
            {
                ++WorkspaceFixedCount;
                FixSource = "workspace config";
            }
            else
            {
                FixSource = "global config";
            }
        }

        const auto Verify = ProbeSafeDirectory(Candidate);
        if (Verify.unsafeOwnership)
        {
            ++Failures;
            std::cerr << kano::terminal::FailTag() << " safe.directory still blocked: " << Candidate.generic_string() << "\n";
            if (!Verify.stderrText.empty())
            {
                std::cerr << FirstLine(Verify.stderrText) << "\n";
            }
            continue;
        }
        std::cout << kano::terminal::PassTag() << " fixed safe.directory (" << FixSource << "): " << Candidate.generic_string() << "\n";
    }

    if (UnsafeCount == 0 && Failures == 0)
    {
        std::cout << kano::terminal::PassTag() << " no safe.directory ownership blockers\n";
    }
    else if (UnsafeCount > 0 && !Fix)
    {
        std::cerr << kano::terminal::WarnTag() << " repair with: kog doctor --repo " << NormalRoot.generic_string() << " --fix-safe-directory\n";
        ++Failures;
    }

    if (Fix && FixedCount > 0)
    {
        std::cout << kano::terminal::InfoTag() << " safe.directory entries added: " << FixedCount << "\n";
    }
    if (Fix && WorkspaceFixedCount > 0)
    {
        std::cout << kano::terminal::InfoTag() << " workspace safe.directory config: " << WorkspaceConfigPath.generic_string() << "\n";
    }

    return Failures == 0 ? 0 : 1;
}

} // namespace

void RegisterDoctor(CLI::App& InApp)
{
    auto* cmd = InApp.add_subcommand("doctor", "Environment and repository health checks");
    auto* RepoRoot = new std::string{"."};
    auto* SafeDirectory = new bool{false};
    auto* FixSafeDirectory = new bool{false};

    cmd->add_option("--repo", *RepoRoot, "Repository root used by repo-scoped doctor checks");
    cmd->add_flag("--safe-directory", *SafeDirectory, "Check workspace repos for Git safe.directory ownership blockers");
    cmd->add_flag("--fix-safe-directory", *FixSafeDirectory, "Add global or workspace-scoped Git safe.directory entries for blocked workspace repos");
    cmd->allow_extras();
    cmd->callback([cmd, RepoRoot, SafeDirectory, FixSafeDirectory]() {
        int failures = 0;
        const auto RepoPath = NormalizePath(std::filesystem::path(*RepoRoot));

        if (*SafeDirectory || *FixSafeDirectory)
        {
            failures += RunSafeDirectoryDoctor(RepoPath, *FixSafeDirectory);
            std::exit(failures == 0 ? 0 : 1);
        }

        auto extras = cmd->remaining();
        if (!extras.empty())
        {
            std::cerr << kano::terminal::WarnTag() << " unrecognized doctor arguments ignored:";
            for (const auto& extra : extras)
            {
                std::cerr << " " << extra;
            }
            std::cerr << "\n";
        }

        failures += CheckCommand("git", "git", {"--version"});
        failures += CheckCommand("inside git repository", "git", {"rev-parse", "--is-inside-work-tree"}, RepoPath);
        failures += CheckCommand("branch", "git", {"branch", "--show-current"}, RepoPath);
        failures += CheckCommand("status", "git", {"status", "--short"}, RepoPath);
        failures += CheckCommand("remotes", "git", {"remote", "-v"}, RepoPath);
        failures += CheckCommand("scalar", "scalar", {"version"});
        failures += CheckCommand("bun", "bun", {"--version"});

        std::exit(failures == 0 ? 0 : 1);
    });
}

} // namespace kano::git::commands