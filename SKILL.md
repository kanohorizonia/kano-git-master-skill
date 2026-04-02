---
name: kano-git-master-skill
description: Comprehensive Git automation toolkit for multi-repository workspaces. Manage root repos plus registered/unregistered subrepos with vendor-agnostic scripts. Quick updates, fork workflows, batch operations, and status reporting.
version: 0.1.0-beta
---

# Kano Git Master Skill

**Version**: 0.1.0-beta
**Status**: Beta Release

Comprehensive Git automation scripts for managing multi-repository workspaces. Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.).

## Quick Start

### Performance (C++ vs Shell vs Native Git)

The native C++ version of `kano-git` provides a significant performance boost over the fallback shell version and native Git commands, especially in complex multi-repository workspaces.

**Test Environment**: 8 repositories (3 registered submodules, 3 unregistered subrepos, 1 nested repo, 1 root).

| Operation | Shell Version | Native Git* | C++ Version (Native) | Speedup (vs Shell) |
| :--- | :--- | :--- | :--- | :--- |
| **Discover Repos** | ~6122ms | N/A | **~337ms** | **~18x faster** |
| **Workspace Status** | ~7976ms | N/A | **~88ms** | **~90x faster** |
| **Foreach Command** | ~2994ms | ~842ms | **~241ms** | **~12x faster** |

*\* Native Git benchmarks use `git submodule foreach --recursive`, which only supports registered submodules. C++ supports both registered and unregistered subrepos natively.*

### Install `kano-git` / `kog` in Git Bash (PATH + completion)

```bash
# From repo root
bash ./scripts/kog-installer

# Apply immediately in current terminal
source ~/.bashrc

# Verify
kano-git --help
kano-git completion bash | head -n 3
```

Installer options:

```bash
# Preview only (no file changes)
bash ./scripts/kog-installer --dry-run

# Use custom rc file
bash ./scripts/kog-installer --rc-file ~/.bash_profile

# Install PATH only (skip completion)
bash ./scripts/kog-installer --no-completion
```

Compatibility alias (same behavior):

```bash
bash ./scripts/kano-git-installer
```

### Native C++ build rule (important)

When building native `kano-git` / `kog`, use the platform scripts under `src/cpp/build/script/`.

```bash
# Windows (recommended)
bash src/cpp/build/script/windows/build_windows_ninja_msvc_release.sh

# Linux (example)
bash src/cpp/build/script/linux/build_linux_ninja_gcc_release.sh

# macOS (example)
bash src/cpp/build/script/macos/build_macos_ninja_clang_release.sh
```

Do not replace this with ad-hoc direct CMake/Ninja command sequences unless a maintainer explicitly asks for it.

### Guided Flows (Easy Win Before Full TUI)

Use the built-in guide command to get practical, low-risk command sequences:

```bash
# List available flows
kano-git guide

# Workspace safe-first flow with checklist
kano-git guide --flow workspace --checklist

# Other flows
kano-git guide --flow sync
kano-git guide --flow commit
kano-git guide --flow worktree
```

This gives you immediate guided UX in pure terminal while we iterate toward richer TUI.

### Global Cross-Repo Status (TUI-first experience seed)

Use top-level status to get one-screen global view across discovered repos:

```bash
# Table view (default)
kano-git status

# JSON view for further tooling
kano-git status --format json

# Narrow scan
kano-git status --max-depth 2 --exclude node_modules --exclude .agents
```

Status includes:
- current branch
- upstream/remote tracking branch
- ahead/behind tracking summary
- repo dirty flag
- dirty worktree detection

### Full TUI Preview (Interactive)

```bash
# Non-interactive preview (for quick validation)
kano-git tui --demo

# Launch interactive dashboard
kano-git tui
```

Current TUI actions (v1):
- left panel: repo list (dirty + branch + path)
- right panel: selected repo details (type, upstream, tracking, dirty/worktree)
- Enter opens incremental history pager for selected repo
- `t`: collapse/expand selected repo subtree in left panel
- `/` in main view: enter repo path filter mode (incremental)
- `c` in main view: show selected repo commit preview (staged/unstaged + risk)
- `C` in main view: commit execute with confirm gate (requires staged files)
- `p` in main view: show selected repo push preview (upstream/tracking + risk)
- `f` in main view: fetch action uses confirm gate (`y` execute / `n` cancel)
- `P` in main view: push execute with confirm gate (requires upstream)
- `x` in main view: cherry-pick preflight panel (source/target candidates, duplicate+risk hints)
- `X` in main view: start cherry-pick runner from preflight non-duplicate queue
- runner controls: `n` next, `c` continue, `s` skip, `a` abort, `q` close panel
- `b` in main view: rebase preflight panel (branch/upstream/divergence/candidate commits/risk)
- `B` in main view: open rebase planner (no execution) from preflight candidates
- planner controls: `↑/↓` select line, `p/s/f/d` set action (pick/squash/fixup/drop), `q` close planner
- `R` in main view: start rebase runner from planner queue
- rebase runner controls: `N` next, `C` continue, `S` skip, `A` abort, `q` close panel
- history mode controls:
  - `←` / `→`: previous/next repo page source
  - `PgUp` / `PgDn`: older/newer history page
  - `↑` / `↓`: move selected commit line in current page
  - `Enter`: open selected commit detail panel (show --stat/--name-status excerpt)
  - `m`: toggle detail view mode (summary / files / patch)
  - `o`: toggle history sort mode (time-desc / time-asc / match-first)
  - `/`: enter history search mode, type keyword, Enter to apply
  - `n`: jump to next match in current page
  - selected history line always shows quick stats (files/insertions/deletions) without opening detail
  - history list has lightweight lane marker (`│`) for scan readability
  - each page loads and caches 20 commit titles on demand
  - title shows repo path, parent repo, child repo count, repo index, and page index
- keyboard controls: `r` refresh, `d` dirty-only toggle, `f` fetch selected repo, `Enter` history pager, `q` quit

### Update Repository + Registered Subrepos (Most Common)

```bash
cd /path/to/your-repo
./scripts/update-repo.sh
```

Updates your repository and all registered subrepos to the latest version with smart branch detection and auto-stash.

### Smart Clone (Clone Fork with Upstream)

```bash
./scripts/smart-clone.sh \
  https://github.com/yourname/fork.git \
  https://github.com/original/repo.git
```

Perfect for contributing to open-source projects.

### Manage Multiple Repositories

```bash
# Discover all repos
./scripts/discover-repos.sh

# Update all repos
./scripts/update-workspace-repos.sh

# Check status
./scripts/status-all-repos.sh
```

## Script Organization

Scripts are organized by category in the `scripts/` directory:

```
scripts/
├── lib/                      # Shared helper library
│   └── git-helpers.sh
├── core/                     # Core operations
│   ├── update-repo.sh
│   ├── smart-clone.sh
│   └── discover-repos.sh
├── workspace/                # Multi-repository operations
│   ├── update-workspace-repos.sh
│   ├── foreach-repo.sh
│   └── status-all-repos.sh
├── branch-operations/        # Branch and commit operations
│   ├── rebase-to-upstream-latest.sh
│   ├── compare-branches.sh
│   └── cherry-pick-batch.sh
└── commit-tools/             # Commit automation
    ├── commit/               # AI-powered commits
    │   ├── smart-commit.sh
    │   └── smart-commit-*.sh (provider wrappers)
    ├── commit-push/          # Complete commit+push workflow
    │   └── smart-commit-push-*.sh
    ├── ignore/               # .gitignore management
    ├── resolve/              # Conflict resolution
    ├── sync/                 # Repository synchronization
    └── smart-push.sh         # Multi-repo push
```

## Terminology (Custom vs Git)

This section defines project-specific terms used in this skill.
If a term is not listed here, assume standard Git meaning.

### Custom terms (this project)

- `stable-dev mode`
  - Sync strategy based on upstream **stable tags**.
  - Create/switch to `branch_<target-tag>`, then cherry-pick maintained commits from previous stable line.

- `dev mode`
  - Sync strategy based on upstream **default branch tip** (not stable tags).
  - Uses the same migration shape as stable-dev but with different upstream base.

- `stable branch`
  - Project convention branch name: `branch_<stable_tag>`.
  - Example: `branch_v1.2.6`.
  - This is a maintenance branch in origin, not a Git built-in branch type.

- `target-tag`
  - The stable tag chosen as the new base in stable-dev sync.

- `base-tag`
  - Previous stable tag used to find prior maintenance branch (`branch_<base-tag>`) for cherry-pick source.

- `bootstrap mode` (stable-dev)
  - Triggered when previous stable source branch is missing and no `--source-branch` is provided.
  - Behavior: create/switch target stable branch from tag, skip cherry-pick, then push.

- `origin-latest` sync
  - Consumer workflow: align to origin branch/tag latest state, no force-push fork maintenance behavior.

- `upstream-force-push` sync
  - Fork-maintenance workflow: sync from upstream then push to origin with `--force-with-lease`.

- `pre-sync` / `post-sync` (smart-commit-push)
  - `pre-sync`: `stash -> sync -> pop` (when stashable) before commit.
  - `post-sync`: sync-only validation after commit, no stash/pop fallback.

- `agent proxy mode (代理模式)`
  - Activated when `--agent <name>` and name is not `manual`.
  - Requires fixed `-m/--message` and auto-disables in-script AI review (`--no-ai-review`).
  - Intent: use the current command agent/model as the single authority (no second review model).

- `workflow lock marker`
  - `.git/kano-smart-commit-push.lock`
  - Indicates workflow in progress; other agents/users should avoid concurrent file edits.

- `protocol-priority=auto` persistence rule
  - `auto` is default behavior and should not be persisted to `.gitmodules`.
  - Persist only explicit non-default (`ssh` or `https`).

### Git official terms (not custom)

- `remote` (`origin`, `upstream`)
- `default branch`
- `detached HEAD`
- `submodule`
- `rebase`
- `cherry-pick`
- `stash`
- `tag`

## Core Scripts

| Script | Purpose | Use When |
|--------|---------|----------|
| **update-repo.sh** | Update single repo + registered subrepos | Daily sync, quick updates |
| **smart-clone.sh** | Clone with upstream remote | Fork setup, contribution workflow |
| **init-empty-repo.sh** | Initialize empty remote repo | Quick repo setup, testing |
| **rebase-to-upstream-latest.sh** | Rebase to upstream | Sync with upstream regularly |
| **discover-repos.sh** | Find all repos in workspace | Multi-repo discovery |
| **update-workspace-repos.sh** | Update multiple repos | Batch updates |
| **foreach-repo.sh** | Run commands in all repos | Custom batch operations |
| **status-all-repos.sh** | Generate status report | Monitoring, CI/CD |
| **compare-branches.sh** | Compare commits between branches | Before merge, PR review |
| **cherry-pick-batch.sh** | Batch cherry-pick from file | Selective commit porting |
| **smart-commit-push** | AI commit+push workflow | Multi-repo commits with AI review and auto-push |

## Common Workflows

### Daily Workspace Sync

```bash
./scripts/workspace/status-all-repos.sh && \
./scripts/workspace/update-workspace-repos.sh && \
./scripts/workspace/foreach-repo.sh "git status --short"
```

### Fork Contribution Workflow

```bash
# Initial setup
./scripts/smart-clone.sh <your-fork> <upstream>

# Regular sync
cd project
../scripts/branch-operations/rebase-to-upstream-latest.sh

# Compare branches before PR
../scripts/branch-operations/compare-branches.sh upstream/main HEAD

# Check commits
../scripts/workspace/foreach-repo.sh "git log upstream/main..HEAD --oneline"
```

### Multi-Repository Management

```bash
# Create manifest
./scripts/repo-management/discover-repos.sh --save workspace-manifest.json

# Update all
./scripts/workspace/update-workspace-repos.sh --manifest workspace-manifest.json

# Check status
./scripts/workspace/status-all-repos.sh --manifest workspace-manifest.json --check-remote
```

## Key Features

### Vendor-Agnostic Design
- ✅ Works with any Git remote provider
- ✅ GitHub, GitLab, Azure Repos, Bitbucket
- ✅ Gitea, Gogs, self-hosted Git servers
- ✅ No platform-specific APIs

### Smart Operations
- ✅ Auto-stash/pop local changes
- ✅ Smart branch detection (current or default)
- ✅ Recursive submodule handling
- ✅ Continue-on-error mode
- ✅ Dry-run preview

### Flexible Discovery
- ✅ Root repositories
- ✅ Registered subrepos from `.gitmodules`
- ✅ Unregistered subrepos in workspace
- ✅ Configurable exclude patterns
- ✅ Manifest file support

### Multiple Output Formats
- ✅ Table (terminal viewing)
- ✅ JSON (CI/CD integration)
- ✅ Markdown (documentation)
- ✅ List (simple output)

## Documentation

- **[Complete Documentation](docs/README.md)** - Full reference with all options
- **[Usage Examples](docs/USAGE-EXAMPLES.md)** - Real-world scenarios and workflows
- **[Quick Reference](docs/QUICK-REFERENCE.md)** - One-page cheat sheet
- **[Testing Guide](TESTING.md)** - Comprehensive testing documentation

## Script Reference

### update-repo.sh (Priority Script)

```bash
# Update current directory
./scripts/update-repo.sh

# Update specific repo
./scripts/update-repo.sh /path/to/repo

# Use different remote
./scripts/update-repo.sh --remote upstream

# Preview changes
./scripts/update-repo.sh --dry-run
```

**Features:** Auto-stash, smart branch detection, recursive registered subrepos, clear progress

### smart-clone.sh

```bash
# Clone without upstream
./scripts/smart-clone.sh https://github.com/user/repo.git

# Clone with upstream
./scripts/smart-clone.sh \
  https://github.com/user/fork.git \
  https://github.com/original/repo.git

# Custom directory
./scripts/smart-clone.sh <url> --dir my-project
```

**Features:** Auto-detect default branch, setup upstream, pull latest, **auto-initialize empty remotes**.

### init-empty-repo.sh

```bash
# Minimal usage - just URL
./scripts/init-empty-repo.sh git@github.com:user/repo.git

# Custom branch
./scripts/init-empty-repo.sh git@github.com:user/repo.git --branch develop

# Custom commit message
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --message "feat: Initial setup"

# Custom file and content
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --file index.html \
  --content "<h1>Hello World</h1>"

# Keep local copy for further work
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --dir ~/my-repo \
  --keep-local

# Force overwrite (DANGEROUS - destroys existing content!)
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --force-overwrite-remote
```

**Features:** AI-friendly (all params optional), sensible defaults, quick initialization, custom content

**Safety Features:**
- Pre-checks if remote already has content
- Refuses to push if remote is not empty (unless forced)
- Verbose flag name (`--force-overwrite-remote`) to prevent accidents
- 3-second warning delay before destructive operations
- Rejects old `--force` flag with helpful error message

### rebase-to-upstream-latest.sh

```bash
# Rebase to upstream/main
./scripts/rebase-to-upstream-latest.sh

# Rebase to upstream/develop
./scripts/rebase-to-upstream-latest.sh --branch develop

# Use origin instead
./scripts/rebase-to-upstream-latest.sh --remote origin
```

**Features:** Root + registered subrepos, auto-stash, configurable remote/branch

### smart-sync* (Sync Shortcuts)

Two explicit sync workflows:

```bash
# 1) Fork workflow: rebase onto upstream then force-push to origin
./scripts/commit-tools/sync/smart-sync-upstream-force-push-copilot.sh

# 2) Consumer workflow: checkout origin default branch and pull --rebase (no push)
./scripts/commit-tools/sync/smart-sync-origin-latest.sh
```

**Practical usage:**
- `smart-sync-upstream-force-push` supports auto-detection:
  - If current repo has `upstream`, operate on current repo.
  - If current repo has no `upstream`, scan workspace and only process repos/subrepos that have `upstream`.
- Use `--repo <path>` to target a specific registered subrepo from workspace root.

```bash
# Auto mode from workspace root: only repos with upstream will run
./smart-sync-upstream-force-push.sh --provider copilot --model gpt-5-mini --verbose

# Target one repo explicitly
./smart-sync-upstream-force-push.sh --repo src/opencode --provider copilot --model gpt-5-mini

# Origin-latest for one repo (no push)
./smart-sync-origin-latest.sh --repo src/opencode

# Origin latest stable release tag (exclude beta/rc by default)
./smart-sync-origin-latest.sh --repo src/opencode --target release

# Include pre-release tags (beta/rc)
./smart-sync-origin-latest.sh --repo src/opencode --target release --release-channel any

# Custom tag regex
./smart-sync-origin-latest.sh --repo src/opencode --target release --tag-pattern '^v?[0-9]+\.[0-9]+\.[0-9]+$'

# Stable-dev mode:
# 1) branch from latest stable tag
# 2) cherry-pick fixes from previous stable dev branch
# 3) push to origin
./smart-sync-stable-dev.sh --repo src/opencode

# Dev mode:
# 1) base on upstream default branch tip
# 2) cherry-pick fixes from origin maintenance branch
# 3) push to origin
./smart-sync-dev.sh --repo src/opencode

# Pin target/base tag explicitly
./smart-sync-stable-dev.sh --repo src/opencode --target-tag v1.0.0 --base-tag v0.9.9

# Explicit source branch (use when branch_<base-tag> does not exist)
./smart-sync-stable-dev.sh --repo src/opencode --target-tag v1.0.0 --base-tag v0.9.9 --source-branch dev/v0.9.9-fixes

# Continue after conflict resolution
./smart-sync-stable-dev.sh --repo src/opencode --continue

# Manual conflict mode (disable AI resolve)
./smart-sync-stable-dev.sh --repo src/opencode --no-ai-resolve
```

**Stable-dev mode selection rule (generic):**
- Repos with `upstream` run stable-dev tag + cherry-pick flow.
- In `stable-dev`, the target line is derived from the latest upstream stable tag first:
  - infer target branch as `branch_<latest-stable-tag>`
  - after successful retarget/sync, write that branch back into the nearest superproject `.gitmodules`
- Repos without `upstream` use fallback branch sync:
  - first read branch from nearest superproject `.gitmodules`
  - if not defined, fallback to remote default branch

**Stable-dev summary output (built-in):**
- `target_version_tag`: release tag used as base (for example `v1.2.6`)
- `maintained_commits_local`: number of local maintenance commits on `target_branch` since `target_version_tag`
- Also includes `planned_commits`, `applied_commits`, `skipped_commits`, and `head_commit`
- If previous source branch (`origin/branch_<base-tag>`) is not found and `--source-branch` is not provided, script enters bootstrap mode:
  - create/switch target branch from stable tag
  - skip cherry-pick
  - push target branch

**Stable-dev precondition (updated):**
- Working tree must be clean.
- Script auto-switches to target stable branch `branch_<target-tag>` (creates it from target tag if missing).

**Conflict behavior:**
- Default: AI auto-resolve during cherry-pick conflicts.
- Manual mode: pass `--no-ai-resolve`, resolve conflicts yourself, then run `--continue`.

**stable-dev vs dev (difference only in upstream base):**
- `stable-dev`: upstream base is latest/previous stable tags.
- `dev`: upstream base is upstream default branch tip (for example `upstream/dev`).
- Both modes use same migration shape: validate maintenance branch -> rebuild target base -> cherry-pick maintenance commits -> push.

**Recommended daily flow (after stable branch line is created):**
1. Run stable migration/update when needed:
   - `./smart-sync-stable-dev.sh --repo src/opencode`
2. Return to daily dev sync:
   - `./smart-sync-origin-latest.sh --repo src/opencode`
3. Verify summary from step 1:
   - check `target_version_tag` and `maintained_commits_local` in `=== Stable Dev Summary ===`

**Root wrapper (`./smart-sync-upstream-stable-dev.sh`) behavior:**
- Without `--repo`, wrapper scans `src/*` registered subrepos and processes only repos with `upstream`.
- End-of-run report is aggregated at the bottom.
- Report formats: `compact` (default), `table`, `tsv`, `json`, `markdown`.

For full operational rules, see:
- `references/ops-policies.md`

### discover-repos.sh

```bash
# Discover all repos
./scripts/discover-repos.sh

# Save to manifest
./scripts/discover-repos.sh --save repos-manifest.json

# JSON output
./scripts/discover-repos.sh --format json

# Filter by type
./scripts/discover-repos.sh --include-types unregistered
```

**Features:** Find root/registered/unregistered subrepos (including nested registered subrepos), exclude patterns, multiple formats, stable JSON output for piping into other tools

### update-workspace-repos.sh

```bash
# Update all repos
./scripts/update-workspace-repos.sh

# Use manifest
./scripts/update-workspace-repos.sh --manifest repos-manifest.json

# Continue on errors
./scripts/update-workspace-repos.sh --continue-on-error

# Filter by type
./scripts/update-workspace-repos.sh --include-types registered
```

**Features:** Batch updates, manifest support, type filtering, error handling

### Canonical workspace state

- Canonical repo inventory/state file: `.kano/cache/git/workspace-manifest.json`
- Current schema uses one top-level `repos` array only.
- Each repo entry carries both inventory fields and scan metadata fields:
  - `path`, `type`
  - `current_branch`, `remotes`, `has_changes`, `dependencies`
- Top-level trust metadata stays separate in `gitmodules_fingerprints`.
- Top-level scan metadata stays separate in `discovery_state`.
- Do not reintroduce duplicated repo arrays under separate manifest/cache sections.

### CPA session behavior note

- Human-mode `./kog cpa` may cause Copilot Chat to create a new chat session for each run.
- Treat this as a current tooling limitation.
- Prefer file-backed prompts and externalized workspace state so CPA does not depend on chat-session continuity.

### CPA hardening lessons

- Human-mode `cpa` and agent-mode `cpa` are materially different paths and should not be documented or debugged as if they were the same execution route.
- For provider-driven plan/commit fill, use file-backed prompts rather than large inline prompt strings.
- Copilot/Codex CLI automation on Windows must account for npm shim executables (`copilot.cmd`, `codex.cmd`).
- Codex non-interactive execution should use `codex exec`.
- The current canonical workspace-state file is `.kano/cache/git/workspace-manifest.json`, while the shared plan file remains `.kano/tmp/git/plans/default-plan.json`.
- The workspace-state schema now keeps one top-level `repos` array only; do not split repo inventory across multiple sections.
- Native commit staging on Windows must guard against reserved device filenames breaking `git add`.

### foreach-repo.sh

```bash
# Check status
./scripts/foreach-repo.sh "git status --short"

# Check unpushed commits
./scripts/foreach-repo.sh "git log origin/main..HEAD --oneline"

# Create branch
./scripts/foreach-repo.sh "git checkout -b feature/new"

# Fetch all
./scripts/foreach-repo.sh "git fetch --all --prune"
```

**Features:** Execute any command, clear output, continue-on-error

### status-all-repos.sh

```bash
# Table report
./scripts/workspace/status-all-repos.sh

# JSON report
./scripts/workspace/status-all-repos.sh --format json

# Check remote status
./scripts/workspace/status-all-repos.sh --check-remote

# Save to file
./scripts/workspace/status-all-repos.sh --format markdown --output STATUS.md

# Detail mode: 5 commits, full log
./scripts/workspace/status-all-repos.sh --detail --detail-commits 5 --detail-log full

# Start from a specific repo root, exclude registered subrepos, non-recursive
./scripts/workspace/status-all-repos.sh --repo-root ./src --no-submodules --no-recursive
```

**Features:** Multiple formats, remote checking, file output, summary stats, last commit time/oneline, configurable detail log mode

### compare-branches.sh

```bash
# Basic comparison
./scripts/branch-operations/compare-branches.sh main feature/new

# Bidirectional comparison
./scripts/branch-operations/compare-branches.sh main develop --bidirectional

# Detailed output with file changes
./scripts/branch-operations/compare-branches.sh main feature/new --detailed

# JSON output
./scripts/branch-operations/compare-branches.sh main feature/new --format json

# Save to markdown file
./scripts/branch-operations/compare-branches.sh main feature/new --format markdown --output diff.md
```

**Features:** Bidirectional comparison, multiple formats (table/JSON/markdown), detailed file changes, commit metadata

### cherry-pick-batch.sh

```bash
# Cherry-pick from JSON file
./scripts/branch-operations/cherry-pick-batch.sh commits.json

# Cherry-pick from text file
./scripts/branch-operations/cherry-pick-batch.sh commits.txt

# Preview without applying
./scripts/branch-operations/cherry-pick-batch.sh commits.json --dry-run

# Continue after resolving conflicts
./scripts/branch-operations/cherry-pick-batch.sh commits.json --continue

# Abort operation
./scripts/branch-operations/cherry-pick-batch.sh commits.json --abort
```

**File formats:**
- JSON: Structured format with hash, title, author, date
- Text: Simple format with hash and optional title
- One hash per line

**Features:** Batch cherry-pick, structured file support, conflict handling, validation

### smart-commit.sh

```bash
# Basic usage (free model)
./scripts/commit-tools/smart-commit.sh

# Use specific model
./scripts/commit-tools/smart-commit.sh --model gpt-5-mini

# Custom message
./scripts/commit-tools/smart-commit.sh -m "feat: Add new feature"

# Commit and push
./scripts/commit-tools/smart-commit.sh --push

# Skip AI review (only static checks)
./scripts/commit-tools/smart-commit.sh --no-ai-review
```

**Features:** AI-generated commit messages, safety checks (secrets, large files), auto .gitignore updates, AI review gate, multi-repo support

## Common Options

All scripts support:
- `--help` - Show help message
- `--dry-run` - Preview mode (no changes)

Most scripts support:
- `--remote <name>` - Remote name (default: origin or upstream)
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type (root,registered,unregistered)
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--continue-on-error` - Don't stop on failures

## Best Practices

1. **Use dry-run first**: Always preview with `--dry-run`
2. **Save manifests**: Create reusable manifests with `--save`
3. **Check status regularly**: Monitor with `status-all-repos.sh`
4. **Continue on errors**: Use `--continue-on-error` for batch operations
5. **Combine scripts**: Chain scripts with `&&` for workflows
6. **Use verbose mode for debugging**: Add `--verbose` to see all repos (default shows only changes)
7. **Review summary tables**: Check commit and push summaries after workflow completion

## Custom Terms Glossary (Non-Git Official Terms)

The following terms are project conventions in Kano Git Master and are **not** official Git terminology:

### KOG ↔ Git Mapping

| KOG term | Git term | Notes |
|---|---|---|
| `kano-git` / `kog` | CLI command alias pair | `kog` is the short alias of `kano-git`; both are supported entrypoints. |
| `root repo` | top-level superproject (contextual) | Workspace boundary owner, typically contains `.gitmodules`. |
| `parent repo` | superproject (relative) | Relationship term used during traversal. |
| `child repo` | submodule (if registered) | Relationship term; use with a specific parent context. |
| `subrepo` | n/a (KOG umbrella) | Any non-root repo. |
| `registered subrepo` | submodule | Declared in root `.gitmodules`. |
| `unregistered subrepo` | nested/independent repo (non-standard) | Not declared in root `.gitmodules`. |
| `leaf repo` | n/a (graph term) | Repo with no child repos in current workspace graph. |

- `smart-*` scripts: Kano automation wrappers that orchestrate multiple Git steps with guardrails.
  - Example: `smart-commit.sh`, `smart-sync.sh`, `smart-push.sh`.
- `kano-git` / `kog`: CLI command aliases for the same tool; `kog` is shorthand of `kano-git`.
- `smart-status`: Kano wrapper name for multi-repo status reporting (implemented via `status-all-repos.sh`).
- `root wrapper`: Thin executable at workspace root that forwards to `.agents/kano/kano-git-master-skill/scripts/...`.
- `root repo`: the top-level repository that owns the workspace boundary and `.gitmodules`.
- `parent repo`: any repo that directly contains another repo path under it in the workspace tree.
- `child repo`: a repo viewed from a specific parent-child relation (topology term, relation-scoped).
- `leaf repo`: a repo that does not contain other child repos in the current workspace graph.
- `subrepo`: umbrella term for non-root repos in the workspace.
- `registered subrepo`: a subrepo declared in root `.gitmodules` (legacy alias: `submodule`).
- `unregistered subrepo`: a subrepo not declared in root `.gitmodules` (legacy alias: `standalone`).
- `child` vs `subrepo`:
  - use `child` / `parent` / `leaf` for topology or traversal operations.
  - use `subrepo` / `registered` / `unregistered` for classification/filtering operations.
- `multi-repo workspace`: A workspace containing a root repo plus registered/unregistered subrepos managed together.
- `stable-dev` flow: Kano-defined maintenance sync mode using stable-tag/cherry-pick strategy (not a Git built-in workflow name).
- `dev` flow (in `smart-sync` context): Kano-defined sync mode based on upstream latest for active development.
- `stable branch` (in stable-dev context): Kano naming convention for maintenance target branch `branch_<target-tag>` (for example `branch_v1.2.6`).
- `target branch` (stable-dev): the branch that will receive cherry-picked maintenance commits; usually equals `stable branch`.
- `source branch` (stable-dev): maintenance commit source branch, defaulting to `origin/branch_<base-tag>` unless overridden by `--source-branch`.
- `target-tag` / `base-tag` (stable-dev): release tag pair used to compute target maintenance line and previous maintenance source baseline.
- `upstream default branch tip` (dev mode): latest commit on upstream default branch used as base in `smart-sync-dev`.
- `AI review gate`: Kano commit gate that evaluates AI/static review verdict before allowing commit/push.
- `agent proxy mode` / `--agent`: Kano execution contract for agent proxy automation identity (for example `codex`, `copilot`).
- `multi-remote push` policy: Kano policy to push to `origin-ssh`, `origin-http`, and `origin` with "any success" semantics.
- `kog-*` keys/commands: Kano-specific namespace (for example `kog-submodule.sh`, `kog-protocol-priority`, `kog-remote-origin-ssh`).
- `smart-submodule`: canonical submodule command entrypoint (`scripts/submodules/smart-submodule.sh`) that dispatches add/sync/update/remove/foreach/sync-urls.
- `include-types`: Kano discovery filter values (`root`, `registered`, `unregistered`) used by workspace scripts (legacy aliases: `submodule`, `standalone`).
- `manifest` (workspace manifest): Kano repo discovery output file consumed by batch scripts (not a Git native artifact).
- `continue-on-error` batch mode: Kano batch execution behavior; continue processing other repos after a per-repo failure.

When writing docs or scripts, use these terms consistently and avoid presenting them as native Git concepts.

## Root Repo Wrapper Script Recommendation

For workspace root scripts (for example `./smart-commit.sh`, `./smart-commit-push.sh`, `./smart-sync.sh`):

1. Keep wrappers thin: only locate project root and `exec` the git-master skill script.
2. Always export `KANO_GIT_MASTER_ROOT="$ROOT"` before `exec` so repo discovery is stable.
3. Put provider choice in wrapper (for example copilot wrapper), but pass through all user args.
4. Do not duplicate business logic in root wrappers; logic must live in `.agents/kano/kano-git-master-skill/scripts/...`.
5. Wrapper names should stay stable and user-facing; internal script path can evolve.

Recommended root wrapper set:
- `smart-commit.sh`
- `smart-commit-push.sh`
- `smart-sync.sh`
- `smart-sync-upstream-stable-dev.sh`
- `smart-status.sh` (recommended when the repository is a multi-repo workspace)

## agent proxy mode (代理模式)

Use agent proxy mode (代理模式) when an agent/tool is executing commit workflows on your behalf.

- Enable with `--agent <name>` (for example: `codex`, `cursor`, `copilot`, `kiro`, `claude`).
- If commands are launched via `kog` / `kano-git`, set `KANO_AGENT_MODE=1` to suppress launcher-level interactive update checks before dispatch.
  - Example: `KANO_AGENT_MODE=1 kog pa --agent codex -m "chore: update workspace"`
- If `--agent` is set and not `manual`:
  - fixed commit message is required: `-m "..."` / `--message "..."`.
  - in-script AI review is disabled automatically (`--no-ai-review`) to avoid duplicate model cost.
  - this keeps commit/review decisions on the current command agent/model.
- Use `--agent manual` for human-operated runs.

Root-wrapper examples:

```bash
./smart-commit.sh --provider copilot --model gpt-5-mini --agent codex -m "chore: sync wrappers"
./smart-commit-push.sh --provider copilot --model gpt-5-mini --agent codex -m "chore: apply stable-dev repair"
```

Ready-to-copy templates:
- Location: `assets/root-wrapper-templates/`
- Profiles:
  - `common/` shared wrappers
  - `profiles/standalone/` (`smart-sync.sh` defaults to `origin-latest`)
  - `profiles/oss/` (includes upstream-focused sync wrappers)
- Generate command (recommended):
```bash
./.agents/kano/kano-git-master-skill/scripts/core/gen-root-wrappers.sh --profile standalone --target .
```

## Plan Review SOP (CLI-only, schema-safe)

For any AI-assisted commit planning or review flow, agents must **not** edit plan JSON directly.
All plan mutations must go through `kog plan` subcommands so plan schema stays valid.

### Current support level

Current supported mode is **single-agent serialized execution only**.

- One agent owns the whole plan-review loop.
- Process commit entries one by one in ascending index order.
- Do not split indexes across multiple agents yet.
- Do not design around claim/lease/parallel worker behavior yet.

Parallel review may be added later, but it is explicitly **out of scope** until serialized `cpa` is stable and repeatable.

### Required rule

- Do **not** rewrite `.kano/tmp/git/plans/*.json` by hand.
- Do **not** ask AI to emit a full replacement plan JSON for commit/review authoring.
- Use native CLI subcommands to inspect and fill commit entries.

### Required inspection/fill loop

1. Count how many commit entries exist and whether any still need review:

```bash
kog plan count-commits --json --plan-file .kano/tmp/git/plans/default-plan.json
```

Expected machine-readable fields:
- `total_commits`
- `review_needed_commits`
- `review_needed_indexes`

2. Print indexed overview:

```bash
kog plan finish-report --plan-file .kano/tmp/git/plans/default-plan.json
```

3. For each target index, inspect the exact entry before updating it:

```bash
kog plan get-commit 0 --json --plan-file .kano/tmp/git/plans/default-plan.json
```

Read-only fields from `get-commit` are the source of truth for:
- `repo`
- `include`
- `exclude`
- current `message`
- current `review`

4. Update only semantic authoring fields via CLI:

```bash
kog plan fill-commit 0 \
  --plan-file .kano/tmp/git/plans/default-plan.json \
  --commit-message "chore: update .agents/kano submodule pointer" \
  --review.verdict pass \
  --review.reason "Workspace dirty state is a tracked gitlink update at .agents/kano and this commit scopes exactly to that change."
```

5. Verify before execution:

```bash
kog plan verify pre-apply --stage commit --plan-file .kano/tmp/git/plans/default-plan.json
```

6. Execute canonical human flow:

```bash
kog cpa
```

### Multi-repo rule

- `fill-commit` and `get-commit` use **global commit index** across all repos in `stages.commit`.
- Agents must not assume `index == repo-local index`.
- Always discover indexes via `count-commits` and `finish-report` first.
- Even in multi-repo plans, review is still serialized by global index order.

### Placeholder / incomplete-plan rule

An entry still needs AI review if any of the following is true:
- empty `message`
- empty `review.verdict`
- empty `review.reason`
- any `replace-with-*` placeholder remains
- `review.verdict` is not `pass`

### `cpa` contract

`kog cpa` must follow the same schema-safe contract:
- tool-owned bootstrap/seed may create the plan structure
- AI may author commit/review content
- commit/review content must be applied through native plan mutation logic, not raw JSON replacement
- verify gates must run before commit execution
- execution model is single-agent serialized until explicitly upgraded

If a future implementation changes internal wiring, it must preserve the external SOP above.

## Troubleshooting

### Azure HTTPS auth: `fatal: unable to get password from user`

Symptom:
- `git ls-remote https://dev.azure.com/...` fails with `fatal: unable to get password from user`
- `git lfs push` fails similarly after switching LFS endpoint to HTTPS

Root cause (common in VS Code agent shells):
- `credential.interactive=never` disables all credential prompts
- `GIT_ASKPASS` points to VS Code askpass in non-interactive contexts

Fix (repo-local, preferred):

```bash
# Allow prompts for this repo only
git config --local credential.interactive always
git config --local credential.https://dev.azure.com.useHttpPath true

# Clear VS Code askpass for current shell
unset GIT_ASKPASS SSH_ASKPASS VSCODE_GIT_ASKPASS_MAIN VSCODE_GIT_ASKPASS_NODE VSCODE_GIT_ASKPASS_EXTRA_ARGS
export GIT_TERMINAL_PROMPT=1

# Re-auth test (enter username + Azure PAT when prompted)
git ls-remote https://dev.azure.com/<org>/<project>/_git/<repo>
```

For Azure Repos + LFS when Git remote stays SSH:

```bash
# Keep git remote as SSH, force LFS over HTTPS endpoint
git config lfs.url https://dev.azure.com/<org>/<project>/_git/<repo>/info/lfs
git config lfs.https://dev.azure.com/<org>/<project>/_git/<repo>/info/lfs.locksverify false
```

Then push in two steps:

```bash
git lfs push origin <branch>
git push
```

Notes:
- If you must change default behavior globally: `git config --global credential.interactive always`
- Prefer repo-local override first to avoid changing other automation environments.

### Stable-Dev Cherry-Pick Repair Playbook (`-X theirs` after sync)

Use this flow when `smart-sync-upstream-stable-dev` bootstraps a stable branch and you must re-apply local maintenance commits:

1. Snapshot local commits first (author/range), then save to a file for traceability.
2. Run stable-dev sync to create/update the target branch.
3. Cherry-pick required commits to target stable branch.
4. If conflicts are too expensive for manual line-by-line resolution, use `git cherry-pick -X theirs <sha>` as a temporary convergence strategy.
5. **Do not stop at green cherry-pick**: run `git range-diff <old-range> <new-range>` to detect semantic drift (`!` entries).
6. Inspect drift hotspots (especially tests and shared infra files) and reconcile manually.
7. Run targeted tests for touched surfaces, then push the stable branch.
8. Record what was repaired (files + reason) so the next stable-dev run has context.

Notes:
- `-X theirs` helps finish conflicted replay quickly, but it is not a correctness guarantee.
- Prefer preserving current stable-branch architecture and only re-applying the intended behavior delta from the original commits.

### Agent Mode SOP: `kog-sync-upstream-stable-dev.sh`

Use this when running stable-dev sync from agent flows (non-interactive, deterministic):

1. Run in agent mode (suppress interactive launcher behavior):

```bash
KANO_AGENT_MODE=1 ./kog-sync-upstream-stable-dev.sh
```

2. If cherry-pick conflicts occur, prioritize convergence first:
  - resolve conflicted hunks with **theirs** (target stable line)
  - `git add ...` then `git rebase --continue` / `git cherry-pick --continue`

3. After all conflicts are green, perform **semantic backfill** (must-do):
  - compare old maintenance intent vs new result (`git range-diff <old-range> <new-range>`)
  - inspect `!` entries and files with behavior drift
  - manually re-apply missing logic from old implementation (do not rely on `-X theirs` output alone)

4. Validate and finalize:
  - run targeted tests for touched surfaces
  - ensure branch report still reflects expected source/target stable lines

5. Push (explicit final step):
  - push only after semantic parity is restored

```bash
git push
# or, if workflow requires explicit remote/branch:
git push origin <target-stable-branch>
```

Quick conflict commands:

```bash
git checkout --theirs <conflicted-file>
git add <conflicted-file>
git rebase --continue   # or: git cherry-pick --continue
```

Required review gate after conflicts:
- "All conflicts resolved" is not done.
- Done = conflicts resolved **and** previous maintenance logic verified/reinstated where needed.

### Rebase Conflicts
```bash
git status                    # Check conflicts
vim conflicted-file.txt       # Resolve
git add conflicted-file.txt   # Mark resolved
git rebase --continue         # Continue
git stash pop stash@{0}       # Restore stash
```

### Stash Recovery
```bash
git stash list                # List stashes
git stash show stash@{0}      # Show content
git stash apply stash@{0}     # Apply
git stash drop stash@{0}      # Drop after success
```

### Detached HEAD
```bash
git checkout -b recovery-branch  # Create branch
# or
git checkout main                # Checkout known branch
```

## Platform Support

- **Linux/macOS**: Works out of the box (Bash 4.0+)
- **Windows**: Use Git Bash for Windows
- **All platforms**: Requires Git 2.x+

## Shared Helper Library

All scripts use `git-helpers.sh` for consistent behavior:

- **Stash management**: Create, pop, check changes
- **Branch operations**: Get current, get default, check existence
- **Repository discovery**: Find repos, collect metadata
- **Remote operations**: Check existence, fetch
- **Utilities**: Logging, dry-run, exclude patterns

Function prefix: `gith_` (git-helper, not GitHub-specific)

## Getting Help

```bash
# Script help
./scripts/repo-management/update-repo.sh --help
./scripts/repo-management/discover-repos.sh --help
./scripts/workspace/update-workspace-repos.sh --help
./scripts/branch-operations/compare-branches.sh --help
./scripts/branch-operations/cherry-pick-batch.sh --help
./scripts/commit-tools/smart-commit.sh --help

# Documentation
cat docs/README.md
cat docs/USAGE-EXAMPLES.md
cat docs/QUICK-REFERENCE.md
```

## New Features

### Branch Comparison (compare-branches.sh)

Compare commits between two branches to understand differences before merging or cherry-picking:

```bash
# See what's in feature branch but not in main
./scripts/branch-operations/compare-branches.sh main feature/new

# Bidirectional comparison
./scripts/branch-operations/compare-branches.sh main develop --bidirectional

# Export to markdown for documentation
./scripts/branch-operations/compare-branches.sh main feature/new \
  --format markdown \
  --output branch-diff.md
```

### Batch Cherry-Pick (cherry-pick-batch.sh)

Cherry-pick multiple commits from a structured file:

**Create commits file (JSON):**
```json
{
  "commits": [
    {
      "hash": "abc123",
      "title": "feat: Add new feature",
      "author": "John Doe",
      "date": "2024-01-15"
    },
    {
      "hash": "def456",
      "title": "fix: Bug fix"
    }
  ]
}
```

**Or simple text format:**
```
abc123 feat: Add new feature
def456 fix: Bug fix
```

**Execute:**
```bash
# Preview first
./scripts/branch-operations/cherry-pick-batch.sh commits.json --dry-run

# Apply
./scripts/branch-operations/cherry-pick-batch.sh commits.json
```

### Smart Commit (smart-commit.sh)

AI-powered commit across all repositories with safety checks:

```bash
# Use Copilot provider (recommended)
./scripts/commit-tools/commit/smart-commit-copilot.sh

# Or use with specific provider
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini

# Custom message for all repos
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini -m "chore: Update dependencies"

# Show detailed output (default: only shows repos with changes)
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini --verbose
```

**Recommended Git config:**
```bash
# Let Git enforce trailing whitespace policy consistently
git config --global core.whitespace trailing-space
```

**Agent proxy mode note (required contract):**
- For agent proxy execution, pass `--agent <name>` (for example: `codex`, `cursor`, `copilot`, `kiro`, `claude`).
- When `--agent` is not `manual`, a fixed commit message (`-m/--message`) is required.
- Agent proxy runs disable in-script AI review (`--no-ai-review`) to avoid duplicate model calls.
- This keeps commit/review authority on the same agent model that executes the command.

**Prompt templates (dev/user mode):**
- AI prompt stages are file-based and mode-aware.
- Default mode is `auto`: `kano-git-master-skill` changes use `dev` prompts, other repos use `user` prompts.
- Override with `--prompt-mode dev|user` or custom template root via `--prompt-root <path>`.
- Environment variable defaults are also supported:
  - `KOG_RULES_TEXT`, `KOG_RULES_FILE`, `KOG_PROMPT_MODE`, `KOG_PROMPT_ROOT`
  - precedence: CLI args > env vars > built-in defaults

**Rule/prompt customization examples:**
```bash
# One-off inline rules
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini --rules "Use release-note friendly summary"

# Rules from file
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini --rules-file ./.rules/team-commit.rule.md

# Force prompt mode and prompt root
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini --prompt-mode user --prompt-root ./.agents/kano/kano-git-master-skill/prompts

# Environment variable defaults
KOG_PROMPT_MODE=user KOG_RULES_FILE=.git/commit-rules.md ./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini
```

**Default rules fallback (built-in):**
- If no explicit `--rules` / `--rules-file` is provided, rule lookup order is:
  1. For `kano-git-master-skill` repo: `dev.rule.md` (`<repo>` -> `<workspace-root>` -> `references/dev.rule.md`)
  2. Commit-convention skill auto-discovery (`skills/**/SKILL.md` + `references/*`)
  3. `default.rule.md` (`<repo>` -> `<workspace-root>` -> `references/default.rule.md`)

**Output modes:**
- **Default (quiet)**: Shows only repos with actual commits
- **Verbose**: Shows all repos, including those with no changes

**Safety features:**
- Detects secrets, API keys, private keys
- Blocks large files
- Auto-updates .gitignore (only prints when file actually changes)
- AI safety review (optional):
  - explicit `FAIL` blocks commit
  - invalid/empty review verdicts are treated as warning and fail-open
- Works with GitHub Copilot CLI

### Smart Commit-Push (smart-commit-push.sh)

Complete workflow: commit and push all repositories in one step:

```bash
# Full workflow with Copilot
./scripts/commit-tools/commit-push/smart-commit-push-copilot.sh

# Or specify provider manually
./scripts/commit-tools/commit-push/smart-commit-push.sh --provider copilot --model gpt-5-mini

# With verbose output
./scripts/commit-tools/commit-push/smart-commit-push.sh --provider copilot --model gpt-5-mini --verbose
```

**Agent proxy mode note (required contract):**
- For agent-driven workflows, pass `--agent <name>` and provide a fixed commit message (`-m "..."`).
- When `--agent` is not `manual`, agent proxy mode (代理模式) disables in-script AI review (`--no-ai-review`) to avoid duplicate model usage.
- This keeps commit/review authority on the same agent model that executes the command.
- For human-run workflows, short flag `-noai` is available (same as `--no-ai-review`).
- If you are modifying this skill itself (`kano-git-master-skill`), commit those edits first before running full `smart-commit-push`.
- Reason: Step 1 pre-sync now uses auto stash/pop, and ongoing edits in the skill repo can be disrupted by stash-pop conflict handling.

**Workflow (4 steps):**
1. **Pre-sync**: auto `stash -> sync -> pop` per repo to rebase/pull safely before committing.
2. **Commit**: runs `smart-commit.sh` across discovered repos.
3. **Post-sync**: `sync-only` again, but **fails if a repo is dirty** (no stash/pop) to prevent pushing an outdated branch tip.
4. **Push**: pushes all repos to configured origin remotes (`origin-ssh`, `origin-http`, and `origin` when present).

**Workspace lock marker (safety):**
- While running, `smart-commit-push` creates a lock directory: `.git/kano-smart-commit-push.lock`.
- Treat this as "hands off": do not edit files during the workflow (especially with multiple agents working).

**Pre-push hooks and `--no-verify`:**
- Some repos/subrepos enforce pre-push hooks (e.g. typecheck). If a hook fails, the overall workflow will fail at Step 4 for that repo.
- If you understand the risk and only need to push, pass `--no-verify` to skip hooks for the `git push` step.

**Multi-remote push success rule (summary):**
- Push attempts all configured origin remotes.
- Repo success = at least one remote succeeds.
- Repo failure = all candidate remotes fail.

**Summary tables:**
After successful completion, displays two summary tables:

1. **Commit Summary**: Shows which repos were committed, how many commits, and branch name
2. **Push Summary**: Shows which repos were pushed, to which remote, and branch name

Example output:
```
=== Commit Summary ===
Repository                    Commits  Branch
-----------                   -------  ------
kano-git-master-skill         1        main
.agents/kano                  1        dev/tooling

=== Push Summary ===
Repository                    Remote             Branch
-----------                   ------             ------
kano-git-master-skill         origin             main
.agents/kano                  origin-http        dev/tooling
backlog                       origin             main (no changes)
```

**Features:**
- Processes root repo, registered/unregistered subrepos, and nested repos
- AI-generated commit messages with safety review
- Automatic sync with upstream before push
- Multi-remote push support (`origin-ssh` + `origin-http` + `origin`)
- Summary statistics on completion

Detailed policy reference:
- `references/ops-policies.md`

## Legacy Scripts

### ai-safe-commit-all-repos.sh (Deprecated)

This script has been replaced by `smart-commit.sh` with improved features:
- Better Copilot CLI detection
- Free tier model defaults
- Clearer error messages
- Simplified naming

Use `smart-commit.sh` instead.

## Summary

The Git Master Skill provides a complete toolkit for multi-repository management:

- **Quick updates**: `update-repo.sh` for fast single-repo updates
- **Fork workflows**: `clone-with-upstream.sh` and `rebase-to-upstream-latest.sh`
- **Multi-repo management**: Discovery, batch updates, status reporting
- **Batch operations**: Execute custom commands across all repos
- **Vendor-agnostic**: Works with any Git provider
- **Production-ready**: Error handling, dry-run, logging, stash management

Start with `update-repo.sh` for simple cases, then explore the full suite for complex workflows.

## Quick Reference

| Need | Command |
|------|---------|
| Update one repo | `./scripts/repo-management/update-repo.sh` |
| Clone fork | `./scripts/smart-clone.sh <fork> <upstream>` |
| Sync with upstream | `./scripts/branch-operations/rebase-to-upstream-latest.sh` |
| Compare branches | `./scripts/branch-operations/compare-branches.sh <base> <compare>` |
| Batch cherry-pick | `./scripts/branch-operations/cherry-pick-batch.sh <file>` |
| Find all repos | `./scripts/repo-management/discover-repos.sh` |
| Update all repos | `./scripts/workspace/update-workspace-repos.sh` |
| Check status | `./scripts/workspace/status-all-repos.sh` |
| Run command | `./scripts/workspace/foreach-repo.sh "command"` |
| Smart commit | `./scripts/commit-tools/smart-commit.sh` |

For detailed examples and workflows, see [docs/README.md](docs/README.md).

## Testing

Comprehensive test suite available:

```bash
# Quick smoke test (30 seconds)
bash scripts/test/quick-test.sh

# Full test suite (5-10 minutes)
bash scripts/test/run-all-tests.sh \
  --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --cleanup
```

See [TESTING.md](TESTING.md) for complete testing documentation.

## Recent Learnings (2026-02-19)

- Add and maintain a passive wrapper profile (`repo-passive-mode`) for multi-device workspaces. In passive mode, wrappers must not auto-init/clone submodules and should default to root + already-cloned submodules only.
- Wrapper templates must include conditional pause behavior: pause only for interactive human runs; do not pause in agent mode (`--agent` non-manual), CI, or non-interactive shells.
- In `smart-commit-push`, post-sync validation should avoid syncing registered submodules again. Re-syncing submodules after commit can advance gitlinks and dirty the root, causing `--fail-on-dirty-sync` failures.
