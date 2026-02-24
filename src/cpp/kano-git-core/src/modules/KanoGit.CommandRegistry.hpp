#pragma once
#include <CLI/CLI.hpp>

namespace kano::git::commands {

void RegisterAll(CLI::App& app);

void RegisterVersion(CLI::App& app);
void RegisterCommit(CLI::App& app);
void RegisterResolve(CLI::App& app);
void RegisterSync(CLI::App& app);
void RegisterPush(CLI::App& app);

void RegisterWorktree(CLI::App& app);
void RegisterSubtree(CLI::App& app);
void RegisterSubmodule(CLI::App& app);
void RegisterScalar(CLI::App& app);

void RegisterP4(CLI::App& app);
void RegisterSvn(CLI::App& app);

void RegisterBranch(CLI::App& app);
void RegisterWorkspace(CLI::App& app);

void RegisterClone(CLI::App& app);
void RegisterDoctor(CLI::App& app);

}
