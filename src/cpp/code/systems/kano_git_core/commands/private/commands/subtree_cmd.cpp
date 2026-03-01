// subtree command — Git subtree management
// Delegates to: scripts/subtree/*.sh

#include "command_registry.hpp"
#include "shell_executor.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SubtreeRecord {
    std::string prefix;
    std::string remote;
    std::string lastSync;
    std::string lastCommit;
};

auto Trim(std::string InValue) -> std::string {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    InValue.erase(InValue.begin(), std::find_if(InValue.begin(), InValue.end(), notSpace));
    InValue.erase(std::find_if(InValue.rbegin(), InValue.rend(), notSpace).base(), InValue.end());
    return InValue;
}

auto JsonEscape(const std::string& InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char ch : InValue) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

auto EnsureGitRepository() -> bool {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"rev-parse", "--git-dir"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode == 0) {
        return true;
    }
    std::cerr << "Error: Not in a git repository\n";
    return false;
}

auto DetectSubtreePrefixes() -> std::vector<std::string> {
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"log", "--grep=^git-subtree-dir:", "--pretty=format:%B"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return {};
    }

    std::set<std::string> unique;
    static const std::regex pattern(R"(git-subtree-dir:\s*([^\s\r\n]+))");
    for (std::sregex_iterator it(result.stdoutStr.begin(), result.stdoutStr.end(), pattern), end; it != end; ++it) {
        unique.insert((*it)[1].str());
    }

    return {unique.begin(), unique.end()};
}

auto GetLastSyncCommit(const std::string& InPrefix) -> std::string {
    const auto grepArg = std::format("--grep=^git-subtree-dir: {}$", InPrefix);
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"log", grepArg, "--pretty=format:%H", "-n", "1"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return "";
    }
    return Trim(result.stdoutStr);
}

auto GetSubtreeSplitRef(const std::string& InPrefix) -> std::string {
    const auto grepArg = std::format("--grep=^git-subtree-dir: {}$", InPrefix);
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"log", grepArg, "--pretty=format:%B", "-n", "1"},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return "unknown";
    }

    static const std::regex splitPattern(R"(git-subtree-split:\s*([^\s\r\n]+))");
    std::smatch match;
    if (std::regex_search(result.stdoutStr, match, splitPattern)) {
        return match[1].str();
    }
    return "unknown";
}

auto GetCommitDate(const std::string& InCommit) -> std::string {
    if (InCommit.empty()) {
        return "Never";
    }
    const auto result = kano::git::shell::ExecuteCommand(
        "git",
        {"log", "-1", "--format=%ai", InCommit},
        kano::git::shell::ExecMode::Capture
    );
    if (result.exitCode != 0) {
        return "Never";
    }
    const auto value = Trim(result.stdoutStr);
    return value.empty() ? "Never" : value;
}

auto CollectSubtrees() -> std::vector<SubtreeRecord> {
    const auto prefixes = DetectSubtreePrefixes();
    std::vector<SubtreeRecord> records;
    records.reserve(prefixes.size());
    for (const auto& prefix : prefixes) {
        const auto lastCommit = GetLastSyncCommit(prefix);
        records.push_back(SubtreeRecord{
            .prefix = prefix,
            .remote = GetSubtreeSplitRef(prefix),
            .lastSync = GetCommitDate(lastCommit),
            .lastCommit = lastCommit.empty() ? "N/A" : lastCommit,
        });
    }
    return records;
}

void PrintSubtreesJson(const std::vector<SubtreeRecord>& InRecords) {
    std::cout << "[\n";
    for (std::size_t i = 0; i < InRecords.size(); ++i) {
        const auto& record = InRecords[i];
        std::cout << "  {\n";
        std::cout << std::format("    \"prefix\": \"{}\",\n", JsonEscape(record.prefix));
        std::cout << std::format("    \"remote\": \"{}\",\n", JsonEscape(record.remote));
        std::cout << std::format("    \"last_sync\": \"{}\",\n", JsonEscape(record.lastSync));
        std::cout << std::format("    \"last_commit\": \"{}\"\n", JsonEscape(record.lastCommit));
        std::cout << "  }";
        if (i + 1 < InRecords.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "]\n";
}

void PrintSubtreesTable(const std::vector<SubtreeRecord>& InRecords) {
    std::cout << std::format("{:<40} {:<50} {:<20}\n", "Prefix", "Remote", "Last Sync");
    std::cout << std::format("{:<40} {:<50} {:<20}\n", "======", "======", "=========");
    for (const auto& record : InRecords) {
        std::string remote = record.remote;
        if (remote.size() > 50) {
            remote = remote.substr(0, 47) + "...";
        }
        std::cout << std::format("{:<40} {:<50} {:<20}\n", record.prefix, remote, record.lastSync);
    }
}

} // namespace

namespace kano::git::commands {

void RegisterSubtree(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("subtree", "Git subtree management");

    auto* add = cmd->add_subcommand("add", "Add a subtree");
    add->allow_extras();
    add->callback([=]() {
        auto extras = add->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/add-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* pull = cmd->add_subcommand("pull", "Pull subtree updates");
    pull->allow_extras();
    pull->callback([=]() {
        auto extras = pull->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/pull-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* push = cmd->add_subcommand("push", "Push subtree changes");
    push->allow_extras();
    push->callback([=]() {
        auto extras = push->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/push-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* split = cmd->add_subcommand("split", "Split subtree");
    split->allow_extras();
    split->callback([=]() {
        auto extras = split->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/split-subtree.sh", args);
        std::exit(result.exitCode);
    });

    auto* list = cmd->add_subcommand("list", "List subtrees");
    list->allow_extras();
    auto* listNative = new bool{false};
    auto* listShell = new bool{false};
    auto* listFormat = new std::string{"table"};
    list->add_flag("--native", *listNative, "Use native C++ subtree list implementation (default)");
    list->add_flag("--shell", *listShell, "Use shell fallback implementation");
    list->add_option("--format", *listFormat, "Output format: table|json");
    list->callback([=]() {
        if (!*listShell) {
            if (*listFormat != "table" && *listFormat != "json") {
                std::cerr << "Error: Unknown format: " << *listFormat << "\n";
                std::exit(1);
            }
            if (!EnsureGitRepository()) {
                std::exit(1);
            }

            const auto records = CollectSubtrees();
            if (records.empty()) {
                if (*listFormat == "json") {
                    std::cout << "[]\n";
                } else {
                    std::cout << "[INFO] No subtrees found in this repository\n";
                }
                std::exit(0);
            }

            if (*listFormat == "json") {
                PrintSubtreesJson(records);
            } else {
                PrintSubtreesTable(records);
            }
            std::exit(0);
        }

        auto extras = list->remaining();
        std::vector<std::string> args(extras.begin(), extras.end());
        auto result = shell::ExecuteScript("subtree/list-subtrees.sh", args);
        std::exit(result.exitCode);
    });
}

} // namespace kano::git::commands
