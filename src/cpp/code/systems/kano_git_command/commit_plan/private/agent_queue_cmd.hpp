#pragma once

#include <CLI/CLI.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace kano::git::commands {

struct ExactPathCommitOptions {
    std::filesystem::path repo;
    std::vector<std::string> paths;
    std::string message;
    std::string expectedHead;
    std::string queueBatch;
    bool dryRun = false;
};

auto RunExactPathCommit(const ExactPathCommitOptions& InOptions) -> int;
void RegisterAgentQueue(CLI::App& InApp);

} // namespace kano::git::commands
