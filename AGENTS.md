# AGENTS.md — kano-git-master-skill

This repo is a **Bash script toolkit** (plus Bash-based tests) for Git automation.

## 1) Build / Lint / Test commands

### Setup / install
- No install step in this repo (no `package.json`, no dependency lockfiles).
- Requirements are documented in `README.md` (Git 2.x+, Bash 4.x+, optional Python/Scalar).

### Build
- No build step (shell scripts only).

### Lint / format
- No repo-provided lint/format commands (no shellcheck/shfmt config found).

### Tests (recommended)
Run from repo root (`skills/kano-git-master-skill/`).

#### Quick smoke test (fast; no external repo required)
```bash
bash scripts/test/quick-test.sh
```

#### Full test suite (clones a real repo; 5–10 min)
```bash
bash scripts/test/run-all-tests.sh \
  --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --cleanup
```

Options are implemented in `scripts/test/run-all-tests.sh`:
- `--test-repo <url>`
- `--cleanup`
- `--verbose`

#### Run a single test script
Most tests are standalone executables in `scripts/test/`:
```bash
bash scripts/test/test-worktree-scripts.sh
bash scripts/test/test-revision-offset.sh
bash scripts/test/test-git-helpers-basic.sh
bash scripts/test/test-create-orphan-branch-properties.sh
```

#### Run one specific test *function* from the full suite
`scripts/test/run-all-tests.sh` defines `test_*` functions. You can source it and call one:
```bash
source scripts/test/run-all-tests.sh
test_compare_branches
```
Note: sourcing will also define globals used by those functions (see the script header).

## 2) Code style & conventions (grounded in this repo)

### Bash scripts (primary language)

#### File header + strict mode
- Use `#!/usr/bin/env bash`.
- Default to strict mode:
  ```bash
  set -euo pipefail
  ```
  Exception: some test runners intentionally avoid strict mode to accumulate failures
  (e.g. `scripts/test/test-input-validation-properties.sh`).

#### Path resolution
- Common pattern for script-relative paths:
  ```bash
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  ```
- **Robust Root Detection**: When a tool needs the project root, avoid `git rev-parse --show-toplevel` if it might be executed from within a submodule (as it will return the submodule root). Instead, use `BASH_SOURCE` to find the script and traverse up to the expected root.
- Prefer `LIB_DIR`/`SKILL_ROOT` computed from `SCRIPT_DIR` and **source** helpers from `scripts/lib/`.

#### Naming
- Functions: `snake_case`.
- Shared library functions in `scripts/lib/git-helpers.sh` use the `gith_` prefix (vendor-agnostic; not “GitHub”).
- Variables:
  - UPPER_CASE for script-level configuration (e.g. `DRY_RUN`, `REPO_URL`, `ORPHAN_BRANCH`).
  - `local lower_case` inside functions.  - **CRITICAL**: `local` keyword can **only** be used inside functions. Never use `local` in script-level for loops or main body. This causes runtime errors in Bash.
#### CLI / `--help` convention
- Scripts with CLI flags generally implement `usage()` that prints a heredoc and exits.
- Argument parsing style:
  ```bash
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --flag) ... ;;
      -h|--help) usage; exit 0 ;;
      *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
    esac
  done
  ```

#### Dry-run behavior
- Many scripts support `--dry-run` via `DRY_RUN=0/1`.
- Prefer using the helper wrapper when appropriate:
  - `gith_run <cmd> [args...]` prints a shell-escaped command when `DRY_RUN=1`, otherwise executes.

#### Logging + errors
- Prefer `gith_log LEVEL MESSAGE` / `gith_error MESSAGE` (from `scripts/lib/git-helpers.sh`).
  - Levels: `INFO`, `WARN`, `ERROR`, `DEBUG`.
  - `DEBUG` output is gated by `GITH_DEBUG=1`.
- Error messages should be **actionable** and go to stderr.
- Validate inputs early (required args, paths exist, “is git repo”, etc.) and return non-zero on failure.

#### Quoting / safety
- Always quote variable expansions: `"$var"`.
- Prefer subshell `cd` for repo-scoped operations:
  ```bash
  (cd "$repo_path" && git status --porcelain)
  ```
- Use arrays for lists (`SCRIPTS=( ... )`) and iterate with `"${arr[@]}"`.

#### Cross-platform intent
- Scripts are intended to work on Unix shells and Git Bash on Windows.
- Avoid relying on GNU-only flags unless already used elsewhere in the repo.

### Tests
- Quick smoke test lives in `scripts/test/quick-test.sh`.
- Full suite orchestrator is `scripts/test/run-all-tests.sh`.
- Many test scripts:
  - create temporary directories via `mktemp -d`
  - manage cleanup explicitly
  - print pass/fail counters and exit non-zero if failures occurred

## Cursor / Copilot repository rules
- No `.cursor/rules/`, `.cursorrules`, or `.github/copilot-instructions.md` found in this repo at time of writing.

## Agent notes (how to make changes that fit)
- Keep changes **minimal and script-local**; avoid broad refactors unless requested.
- When adding a new script:
  - place it under an existing category in `scripts/`
  - include `--help` and `--dry-run` when it mutates state
  - source `scripts/lib/git-helpers.sh` for consistent logging + helpers
- **Bash Variable Scope Rules**:
  - Use `local` **only inside functions**, never in script-level for loops or main body
  - In for loops at script level, declare variables without `local` keyword
  - Common mistake: `local var="$(command)"` in a for loop → runtime error
  - Correct pattern in loops: `var="$(command)"` (no `local`)
- **Smart-tools Output Philosophy**:
  - Default: show only repos with actual changes/operations (quiet mode)
  - Add `--verbose` flag to show all repos including no-change cases
  - Prevents noise when processing many repos with few changes
  - Print status changes (e.g., `.gitignore updated`) only when file actually modified
  - Use MD5 hash comparison or git diff to detect real changes before printing
- **Statistics & Summaries**:
  - Collect operation stats in arrays throughout execution
  - Display summary tables at completion showing: repo name, operation count, target (branch/remote)
  - Format tables with `printf` for alignment: `printf "%-35s  %-7s  %s\n"`
  - Stats format: `"repo_name|count|target"` (pipe-delimited for easy parsing)
- **Submodule Management**:
  - Always align submodules to the branch specified in `.gitmodules` during sync. Use `gith_checkout_branch` to handle detached HEADs and remote tracking safely.
  - Support flexible remote naming (e.g., `kog-remote-upstream`) without forcing protocol suffixes if a direct URL is provided.
- **Root-Level Wrapper Pattern**:
  - For complex multi-repo tools (e.g., `smart-commit`), provide simple entry-point wrappers in the project root.
  - This ensures "Git Bash Here" works correctly and provides a stable, context-aware interface for developers.
- **AI Safety & Authentication**:
  - Scripts like `smart-commit.sh` perform AI safety reviews. Note that **Copilot authentication has two layers**: the standalone CLI (`copilot login`) and the GitHub CLI extension (`gh auth login`).
  - **Commit Message Filtering**: The system automatically filters AI conversational preamble (e.g., "Certainly!", "I'll inspect..."). It prioritizes lines matching **Conventional Commits** (`type(scope): msg`) or **Bracketed Tags** (`[Tag][SubTag] msg`).
  - **Agent Proxy Contract (Required)**: For agent-proxy `smart-*` operations, pass `--agent <name>` to declare identity (e.g., `codex`, `copilot`, `cursor`, `kiro`, `claude`).
  - If `--agent` is provided and not `manual`, agent proxy mode (代理模式) is active: a fixed message (`-m/--message`) is required and in-script AI review is disabled (`--no-ai-review`) to avoid duplicate model cost.
  - Intent: commit/review decisions stay on the same agent model executing the command, not a second model pipeline.
  - Use `--agent manual` for human-operated runs.
  - **In case of Copilot auth failure, seek human assistance** to resolve credentials. Do not bypass reviews if a reliable safety check is possible.
- **Kano Backlog Init Location**:
  - Run `kano backlog admin init` from `_kano/backlog` to generate `.kano/config` for this repo.
- When adding/adjusting behavior, update the relevant docs in `docs/` and add/extend tests in `scripts/test/`.

## Recent learnings (2026-02)

### `discover-repos.sh` JSON stability
- Symptom: malformed JSON objects like `{,"type":"registered"}` caused downstream parsers to warn/skip.
- Root cause: nested submodules were emitted without parent path prefix (wrong absolute path), and invalid metadata `{}` was later string-appended with `"type"`, producing invalid JSON.
- Fix pattern:
  - When recursing `gith_collect_submodules()`, prefix nested paths with `"$parent/$child"` (not child-only).
  - When emitting JSON in `gith_discover_repos()`, validate each candidate path is a git repo and skip invalid metadata (empty/`{}`) before appending `"type"`.
- Dev note: treat JSON building as structured, not “string concat on faith”.

### Auto-stash edge case in multi-repo sync
- `git status --porcelain` can report changes for a repo that are not stashable (common when only the submodule pointer state is “dirty”).
- `git stash push` may print `No local changes to save` even when the caller considered the repo “dirty”.
- Fix pattern: only `stash pop` when a stash was actually created (detect via command output or stash list), and keep recovery instructions when pop fails.

### Shell vs PowerShell redirection
- This toolkit is Bash-first (Git Bash on Windows). If you are debugging from PowerShell, note that `/dev/null` redirection is not valid there (`2>$null` is the PowerShell equivalent).

### `smart-sync` split (explicit workflows)
- Goal: avoid one ambiguous "sync" command that users run in the wrong context (fork vs consumer).
- New workflows:
  - `scripts/commit-tools/sync/smart-sync-upstream-force-push.sh`: AI rebase onto upstream default branch, then `git push --force-with-lease` to origin.
  - `scripts/commit-tools/sync/smart-sync-origin-latest.sh`: checkout origin default branch and `git pull --rebase` (no push).
- Project root provides wrappers (`smart-sync.sh` dispatches by mode) to keep usage stable from the workspace root.

### Prompt templates (AI stages)
- `smart-commit.sh` now uses file-based prompt templates for `commit-message` and `review` stages.
- Template layout: `prompts/base/<stage>.md` + optional overlay `prompts/<mode>/<stage>.md`.
- Mode routing:
  - `auto` (default): `kano-git-master-skill` repos -> `dev`, others -> `user`
  - override with `--prompt-mode dev|user`
- Keep templates short and policy-focused; repo/file stats and diff previews are appended by script runtime.

### Stable-dev wrapper scope + reporting
- `smart-sync-upstream-stable-dev.sh` default scope is `src/*` submodules only.
- Wrapper only executes repos that have `upstream` remote; no-upstream repos are skipped.
- End-of-run reporting should be aggregated at the bottom (not interleaved across repos).
- Commit report payload standard:
  - `current_branch`
  - `latest_upstream_commit`
  - `latest_stable_branch_commit`
  - commit line format: `sha | commit-time | author | title`
- Report format switch is now wrapper-owned (`--format compact|table|tsv|json|markdown`) and must not leak to inner sync script args.

### Stable-dev branch bookkeeping
- Stable-dev sync updates superproject `.gitmodules` branch for target submodule branch (for example `branch_v1.2.6`).
- This keeps future detached-head recovery aligned with intended maintenance branch.

### Multi-remote push semantics
- `smart-push.sh` now pushes to all available origin remotes:
  - `origin-ssh`
  - `origin-http`
  - `origin`
- Success policy is "any success": repo push is successful if at least one remote push succeeds.
- Repo is failed only when all candidate origin remotes fail.
- Non-verbose mode suppresses partial-remote failure noise; verbose mode prints per-remote failures.

### `kog-protocol-priority` persistence rule
- `kog-protocol-priority=auto` should not be written by default.
- Persist `kog-protocol-priority` only when user explicitly chooses non-default value (`ssh` or `https`).

### AI review gate robustness
- `smart-commit.sh` AI review gate is fail-open for provider/format glitches:
  - Empty/invalid verdict -> warning + continue
  - Explicit `FAIL` verdict -> block commit
- Parsing tolerates common small-model output drift (`PASS - ...`, leading spaces, fuzzy PASS/FAIL tokens).
