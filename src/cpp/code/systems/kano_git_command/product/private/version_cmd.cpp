// version command — prints version information

#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(KOG_USE_MODULES)
import kano.git.version;
#else
#include "version.hpp"
#endif

namespace kano::git::commands {

namespace {

auto JsonEscape(const std::string_view InValue) -> std::string {
    std::string out;
    out.reserve(InValue.size() + 16);
    for (const char c : InValue) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

auto BuildInfoEntries() -> std::vector<std::pair<std::string, std::string>> {
    return {
        {"version", std::string(kano::git::GetBuildVersion())},
        {"vcs", std::string(kano::git::GetBuildVCS())},
        {"branch", std::string(kano::git::GetBuildBranch())},
        {"rev", std::string(kano::git::GetBuildRevision())},
        {"hash_short", std::string(kano::git::GetBuildRevisionHashShort())},
        {"hash", std::string(kano::git::GetBuildRevisionHash())},
        {"dirty", std::string(kano::git::GetBuildDirty())},
        {"host", std::string(kano::git::BuildHostName())},
        {"host_platform", std::string(kano::git::BuildHostPlatform())},
        {"toolchain", std::string(kano::git::GetBuildToolchain())},
        {"ci", std::string(kano::git::GetBuildCI())},
        {"pipeline", std::string(kano::git::GetBuildPipelineId())},
    };
}

auto BuildInfoJson() -> std::string {
    const auto entries = BuildInfoEntries();
    std::string out = "{";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "\"" + JsonEscape(entries[i].first) + "\":\"" + JsonEscape(entries[i].second) + "\"";
    }
    out += "}";
    return out;
}

auto WriteFile(const std::filesystem::path& InPath, const std::string& InContent) -> bool {
    std::error_code ec;
    if (InPath.has_parent_path()) {
        std::filesystem::create_directories(InPath.parent_path(), ec);
    }
    std::ofstream out(InPath, std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << InContent;
    out << "\n";
    return static_cast<bool>(out);
}

} // namespace

void RegisterVersion(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("version", "Show version information");
    auto* format = new std::string{"plain"};
    auto* writeFilePath = new std::string{};
    cmd->add_option("--format", *format, "Output format: plain|json");
    cmd->add_option("--write-file", *writeFilePath, "Write build info output to file path");

    cmd->callback([=]() {
        std::string output;
        if (*format == "plain") {
            output = kano::git::GetBuildInfo();
        } else if (*format == "json") {
            output = BuildInfoJson();
        } else {
            std::cerr << "ERROR: Unsupported --format: " << *format << " (supported: plain, json)\n";
            std::exit(2);
        }

        std::cout << output << "\n";

        if (!writeFilePath->empty()) {
            const std::filesystem::path outPath(*writeFilePath);
            if (!WriteFile(outPath, output)) {
                std::cerr << "ERROR: Failed to write file: " << outPath.generic_string() << "\n";
                std::exit(1);
            }
            std::cout << "file_written=" << outPath.generic_string() << "\n";
        }
    });
}

} // namespace kano::git::commands
