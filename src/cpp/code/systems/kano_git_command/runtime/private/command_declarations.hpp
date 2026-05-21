#pragma once

#include <CLI/CLI.hpp>

namespace kano::git::commands {

void RegisterVersion(CLI::App& InApp);
void RegisterCache(CLI::App& InApp);
void RegisterConfig(CLI::App& InApp);
void RegisterMeta(CLI::App& InApp);
void RegisterComplete(CLI::App& InApp);
void RegisterCompletion(CLI::App& InApp);
void RegisterDiscover(CLI::App& InApp);
void RegisterWorkspace(CLI::App& InApp);
void RegisterForeach(CLI::App& InApp);
void RegisterGuide(CLI::App& InApp);
void RegisterLog(CLI::App& InApp);
void RegisterRemote(CLI::App& InApp);
void RegisterStatus(CLI::App& InApp);
void RegisterTui(CLI::App& InApp);
void RegisterUpdate(CLI::App& InApp);
void RegisterCommit(CLI::App& InApp);
void RegisterCommitPush(CLI::App& InApp);
void RegisterAmend(CLI::App& InApp);
void RegisterResolve(CLI::App& InApp);
void RegisterSelf(CLI::App& InApp);
void RegisterSync(CLI::App& InApp);
void RegisterPush(CLI::App& InApp);
void RegisterConverge(CLI::App& InApp);
void RegisterRepo(CLI::App& InApp);
void RegisterReset(CLI::App& InApp);
void RegisterWorktree(CLI::App& InApp);
void RegisterSubtree(CLI::App& InApp);
void RegisterSubmodule(CLI::App& InApp);
void RegisterScalar(CLI::App& InApp);
void RegisterP4(CLI::App& InApp);
void RegisterPlan(CLI::App& InApp);
void RegisterSlog(CLI::App& InApp);
void RegisterSvn(CLI::App& InApp);
void RegisterUplog(CLI::App& InApp);
void RegisterBranch(CLI::App& InApp);
void RegisterClone(CLI::App& InApp);
void RegisterDoctor(CLI::App& InApp);
void RegisterExport(CLI::App& InApp);
void RegisterFetch(CLI::App& InApp);
void RegisterIgnore(CLI::App& InApp);
void RegisterCherryPick(CLI::App& InApp);
void RegisterStash(CLI::App& InApp);
void RegisterRepoHygiene(CLI::App& InApp);

} // namespace kano::git::commands
