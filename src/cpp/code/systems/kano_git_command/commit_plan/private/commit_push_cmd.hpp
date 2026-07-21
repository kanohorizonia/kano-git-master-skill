#pragma once

#include <functional>
#include <memory>
#include <string>

namespace CLI {
class App;
}

namespace kano::git::commands {

struct CommitPushCommandOptions {
    std::string repos;
    bool noRecursive = false;
    std::string message;
    std::string commitPlanFile;
    bool writeCommitPlanTemplate = false;
    std::string commitPlanOut;
    std::string aiProvider;
    std::string aiModel;
    std::string aiFillMode;
    bool aiAuto = false;
    bool noAiReview = false;
    bool stagedOnly = false;
    bool dryRun = false;
    bool profile = false;
    std::string branchMode = "default";
    bool forceWithLease = false;
    bool noVerify = false;
    int jobs = 0;
    bool verbose = false;
    std::string remote;
    std::string repoRoot;
    std::string target;
    bool yolo = false;
};

auto MakeCommitPushCommandCallback(CLI::App& InCommand,
                                   const std::shared_ptr<CommitPushCommandOptions>& InOptions)
    -> std::function<void()>;

} // namespace kano::git::commands
