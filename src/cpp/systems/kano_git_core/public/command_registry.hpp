#pragma once
#include <CLI/CLI.hpp>

namespace kano::git::commands {

void RegisterAll(CLI::App& InApp);

void RegisterVersion(CLI::App& InApp);
void RegisterCommit(CLI::App& InApp);
void RegisterResolve(CLI::App& InApp);
void RegisterSync(CLI::App& InApp);
void RegisterPush(CLI::App& InApp);

void RegisterWorktree(CLI::App& InApp);
void RegisterSubtree(CLI::App& InApp);
void RegisterSubmodule(CLI::App& InApp);
void RegisterScalar(CLI::App& InApp);

void RegisterP4(CLI::App& InApp);
void RegisterSvn(CLI::App& InApp);

void RegisterBranch(CLI::App& InApp);
void RegisterWorkspace(CLI::App& InApp);

void RegisterClone(CLI::App& InApp);
void RegisterDoctor(CLI::App& InApp);

}
