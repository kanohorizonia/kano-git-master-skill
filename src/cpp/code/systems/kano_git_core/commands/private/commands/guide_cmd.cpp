// guide command - lightweight guided UX entrypoint

#include "command_registry.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace kano::git::commands {
namespace {

std::string BuildFlowGuide(const std::string& flow) {
    if (flow == "update" || flow == "discover" || flow == "foreach") {
        return R"TXT(Guide: update/discover/foreach (safe first)

1) Preview repository discovery and waves
   kano-git discover --emit-waves

2) Preview update plan only (no execution)
   kano-git update --native-plan-only --native-max-depth 3

3) Execute update with controlled parallelism
   kano-git update --native --parallel 2 --continue-on-error

4) Run a command across repos with native planner
   kano-git foreach --native --parallel 2 --command "git status --short"
)TXT";
    }

    if (flow == "sync") {
        return R"TXT(Guide: sync (fork workflows)

1) Consumer sync to origin latest
   kano-git sync origin-latest

2) Fork maintenance sync from upstream (force-with-lease)
   kano-git sync upstream-force-push

3) Stable maintenance line sync
   kano-git sync stable-dev

Tip: Start with origin-latest if unsure.
)TXT";
    }

    if (flow == "commit") {
        return R"TXT(Guide: commit (AI-assisted)

1) Generate commit message automatically
   kano-git commit

2) Use explicit provider/model
   kano-git commit --provider opencode --model gpt-5

3) Proxy mode with fixed message
   kano-git commit --agent codex -m "feat: ..."

4) Commit then push
   kano-git commit --push
)TXT";
    }

    if (flow == "worktree") {
        return R"TXT(Guide: worktree

1) List worktrees (native)
   kano-git worktree list --native --format table

2) Create worktree
   kano-git worktree create <branch-name>

3) Open worktree
   kano-git worktree open <branch-name>

4) Remove worktree
   kano-git worktree remove <branch-name>
)TXT";
    }

    return "";
}

void PrintGeneralGuide() {
    std::cout
        << "kano-git guide (quick start flows)\n\n"
        << "Available flows:\n"
        << "  - discover   (repo discovery + manifest refresh)\n"
        << "  - foreach    (run a command across repos)\n"
        << "  - update     (multi-repo update planner/executor)\n"
        << "  - sync       (origin/upstream/stable-dev sync strategies)\n"
        << "  - commit     (AI-assisted commit workflows)\n"
        << "  - worktree   (create/list/open/remove worktrees)\n\n"
        << "Show one flow:\n"
        << "  kano-git guide --flow update\n\n"
        << "Include quick safety checklist:\n"
        << "  kano-git guide --flow update --checklist\n";
}

void PrintChecklist() {
    std::cout
        << "\nChecklist before execution:\n"
        << "  [ ] Confirm target branch/remotes\n"
        << "  [ ] Run plan-only preview if available\n"
        << "  [ ] Use --dry-run when uncertain\n"
        << "  [ ] Keep --continue-on-error intentional\n";
}

} // namespace

void RegisterGuide(CLI::App& InApp) {
    auto* cmd = InApp.add_subcommand("guide", "Show guided workflows for common tasks");

    auto* flow = new std::string{};
    auto* checklist = new bool{false};

    cmd->add_option("--flow", *flow, "Flow name: discover|foreach|update|sync|commit|worktree");
    cmd->add_flag("--checklist", *checklist, "Append safety checklist");

    cmd->callback([flow, checklist]() {
        if (flow->empty()) {
            PrintGeneralGuide();
            if (*checklist) {
                PrintChecklist();
            }
            return;
        }

        const std::string content = BuildFlowGuide(*flow);
        if (content.empty()) {
            std::cerr << "Unknown flow: " << *flow
                      << " (expected: discover|foreach|update|sync|commit|worktree)\n";
            std::exit(2);
        }

        std::cout << content;
        if (*checklist) {
            PrintChecklist();
        }
    });
}

} // namespace kano::git::commands
