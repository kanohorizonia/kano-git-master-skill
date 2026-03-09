# AGENTS.md — kano-git-master-skill

This repo is a **Bash script toolkit** (plus Bash-based tests) for Git automation.

## 1) Build / Lint / Test commands

### Setup / install
- No install step in this repo (no `package.json`, no dependency lockfiles).
- Requirements are documented in `README.md` (Git 2.x+, Bash 4.x+, optional Python/Scalar).

### Build
- Shell scripts: no build step.
- C++ native CLI (`kano-git`/`kog`): **must** use platform build scripts under `src/cpp/build/script/`.
  - Windows (recommended):
    - `bash src/cpp/build/script/windows/build_windows_ninja_msvc_release.sh`
  - Linux (example):
    - `bash src/cpp/build/script/linux/build_linux_ninja_gcc_release.sh`
  - macOS (example):
    - `bash src/cpp/build/script/macos/build_macos_ninja_clang_release.sh`
- Do not use ad-hoc direct CMake/Ninja command sequences when working in this repo unless explicitly requested by a human maintainer.

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
- **Bootstrap-only scripts policy (required)**:
  - `scripts/` (especially `scripts/kano-git` and `scripts/kog`) are bootstrap/entry wrappers only.
  - Do not add new business pipeline logic to shell wrappers when the same logic can live in native C++ commands.
  - Wrapper responsibilities are limited to:
    - locating/dispatching native binary
    - minimal environment/bootstrap setup
    - installer/completion/wrapper-generation glue
  - Commit/plan/verify/apply/sync/push decision logic must be implemented in native C++ (`src/cpp/...`), not duplicated in shell.
  - If a behavior currently exists in shell and is not bootstrap-critical, migrate it into native first, then simplify/remove shell path.
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
- **Launcher agent env (important)**: when invoking through `kog`/`kano-git`, set `KANO_AGENT_MODE=1` for agent runs so launcher update prompts/interactive flows are suppressed before command dispatch.
  - Example: `KANO_AGENT_MODE=1 kog pa --agent codex -m "chore: update workspace"`
- Intent: commit/review decisions stay on the same agent model executing the command, not a second model pipeline.
- **Agent-mode AI ownership rule (kog)**:
  - In `KANO_AGENT_MODE=1`, launcher treats `cpa` as `cp` (no extra launcher-side AI preflight injection).
  - AI planning/review is owned by the externally started agent session; `kog` should execute deterministic gates/pipeline only.
  - Use `-a/--agent <name>` explicitly for agent runs so identity is recorded and the correct agent-mode path is forced.
  - Even in agent mode, plan completeness gates still apply; if prepared fields are missing/invalid, verify must fail fast (do not bypass).
- Use `--agent manual` for human-operated runs.
  - **In case of Copilot auth failure, seek human assistance** to resolve credentials. Do not bypass reviews if a reliable safety check is possible.
  - **Human-mode CPA no-fallback rule (required)**: when `cpa` runs in human mode with `commit_generation_mode=single`, AI commit generation must fail fast on invalid/empty fill output.
  - Do not inject deterministic fallback commit messages, do not auto-repair empty commit stages with fallback commit entries, and do not route around the failure by silently continuing the pipeline.
  - Expected behavior is a surfaced error with debug evidence so the operator can inspect the AI failure directly.
- **Skill developer dogfood rule (required)**:
  - If you are developing this skill itself, execute by the skill design intent first; do not bypass missing/buggy parts with ad-hoc shortcuts.
  - When a gap/bug is found, prioritize implementing or fixing the feature, then continue the operator workflow.
  - Avoid "work around and move on" behavior for core pipeline stages (plan/verify/apply/sync/push).
  - Execution checklist for skill developers:
    - Reproduce with the canonical command path first (no side-step command aliases).
    - If behavior deviates from design intent, implement/fix before proceeding.
    - Re-run the same canonical path to confirm fix; record evidence in task worklog.
    - Only use temporary bypass flags for one-off diagnostics; remove them from normal flow.
- **AGENT MODE sync conflict SOP (required)**:
  - On sync/rebase conflict, stop the auto flow and preserve evidence (`git status`, conflict file list, current SHA).
  - Prefer integrating remote latest state first ("theirs-first" for conflict baseline), then replay local intended changes explicitly.
  - After conflict resolution, run plan verification gates again before continuing (`plan verify pre-apply`, then post-apply checks).
  - Do not silently drop local planned changes; ensure each planned commit intent is present after merge/replay.
- **Plan dependency ordering + parallelization policy**:
  - Plan commits should be topologically ordered by dependency graph before execution.
  - Minimum graph: parent repo/submodule pointer dependencies must commit after child repo commits.
  - Only independent nodes (no dependency edge) may run in parallel; dependent nodes must run in later waves.
  - If graph ordering is violated and causes a second pass, treat it as a pipeline defect and fix the planner/executor.
  - Graph construction baseline:
    - Node = one plan commit item (`repo + commit index`).
    - Edge types:
      - `submodule-pointer`: superproject commit depends on submodule commit that updates referenced SHA.
      - `same-repo-sequence`: later commit in same repo depends on earlier commit when file sets overlap or explicit ordering is declared.
      - `cross-repo-artifact`: commit consuming generated artifact depends on producer commit.
  - Execution model:
    - Run topo layers in waves (`wave0`, `wave1`, ...).
    - Parallelize within a wave only.
    - Persist wave/order metadata into plan execution report for audit and replay.
- **Kano Backlog Init Location**:
  - Run `kano backlog admin init` from `_kano/backlog` to generate `.kano/config` for this repo.
- When adding/adjusting behavior, update the relevant docs in `docs/` and add/extend tests in `scripts/test/`.

## Recent learnings (2026-02)

## Recent learnings (2026-03)

### Agent-mode `cpa` ownership contract
- Human `cpa` and agent-mode `cpa` are not the same path.
- Human path:
  - `cpa` -> `commit-push --ai-auto`
- Agent path:
  - `KANO_AGENT_MODE=1 cpa` must use shared plan-file execution, not internal provider-driven plan generation.
- Required contract:
  - external agent owns semantic authoring decisions
  - `kog` owns deterministic gates, plan bootstrap/refresh, verify/apply/sync/push
  - agent mode must not silently fall back to human `--ai-auto` behavior
- If agent mode would invoke internal AI provider subprocesses, treat it as a workflow defect and fix the route rather than working around it.

### Deterministic plan metadata belongs to the tool, not the agent
- Fields such as:
  - `meta.plan_id`
  - `meta.generated_at_utc`
  - `meta.planner.provider`
  - `meta.planner.ai-model`
  - deterministic `review.reason`
- should be generated by native plan tooling during `plan new` / deterministic seed/bootstrap.
- Agent intervention should be reserved for actual semantic content, not boilerplate metadata repair.
- If a workflow still requires hand-editing placeholder metadata to make `cpa` executable, treat that as tooling incompleteness.

### Shared default-plan behavior for agent mode
- Shared plan location:
  - `.kano/tmp/git/plans/default-plan.json`
- In agent mode, `commit-push` should:
  - run freshness check
  - refresh/bootstrap the shared plan when stale or missing
  - continue through deterministic verification and execution
- Plan drift should be solved by tool-owned refresh, not by telling the operator to manually rerun extra plan commands.

### Multi-repo ordering contract is asymmetric by design
- `sync` serial policy is parent-first.
- `commit` / `commit-push` execution must be child-first for nested repos.
- Rationale:
  - `sync`: parent branch / `.gitmodules` state should settle before registered child handling
  - `commit`: child repos must settle before parent gitlink commits
- If logs show nested parent/child repos committing in the same wave, that is a real dependency-graph defect, not just a logging issue.

### First-run correctness beats retry-based convergence
- Do not normalize "first run fails, second run fixes it" as acceptable `cpa` behavior.
- Investigate the first failing stage directly:
  - commit ordering
  - sync ordering
  - post-sync gitlink convergence
  - push failure reporting
- Retry logic is only acceptable after first-run failure modes are already understood and explicitly justified.

### Post-sync gitlink handling must not rely on automatic force push
- Auto `force push`, including `--force-with-lease`, is not an acceptable generic repair path for `cpa`.
- If post-sync gitlink updates require history rewriting, first verify why the parent repo was not still in a safe unpublished state.
- Preferred model:
  - amend only when it is provably still local-only and safe
  - otherwise create a follow-up commit
- Never hide a pipeline design bug behind automatic force-push behavior.

### Manifest-first discovery model
- High-frequency commands should prefer workspace manifest/cache as the primary source of repo inventory.
- Invalidation should be coarse and explicit:
  - trust manifest/cache while valid
  - on untrusted state, do a full scan
- Registered repos:
  - source of truth is recursive `.gitmodules`
- Unregistered repos:
  - if introduced by `kog clone`, update manifest directly
  - if introduced by manual `git clone`, require explicit `kog discover` full scan to import them
- Avoid rebuilding a complex partial invalidation graph when a manifest-trust + full-scan fallback model is sufficient.

### Cache writes need cross-process coordination
- Shared runtime state under `.kano/tmp/git/*` and discovery cache under `.kano/cache/git/discover-repos/*` require:
  - atomic write (temp file + rename)
  - per-resource cross-process lock
  - stale-lock cleanup path
- Default shared resources include:
  - workspace manifest under `.kano/tmp/git/`
  - discover cache under `.kano/cache/git/discover-repos/`
  - shared default plan under `.kano/tmp/git/plans/`
- Prefer lock files/directories plus cleanup/inspection commands over broad global locks or implicit best-effort writes.

### Dogfood evidence should be visible without source archaeology
- User-facing guide/help should expose important workflow contracts such as:
  - `sync` parent-first
  - `commit` child-first
- Failure output should print the real git/subprocess reason instead of only a summary line.
- If behavior can only be explained by reading source or sampling a stuck process, logging/help coverage is still insufficient.

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
- Template layout: `assets/prompts/base/<stage>.md` + optional overlay `assets/prompts/<mode>/<stage>.md`.
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

### Azure HTTPS credential prompt blocked (`unable to get password from user`)
- Symptom: `git ls-remote https://dev.azure.com/...` and `git lfs push` fail with `fatal: unable to get password from user`.
- Root cause: global/user config had `credential.interactive=never`, which suppresses all prompts; VS Code `GIT_ASKPASS` may also keep shell non-interactive.
- Fast repo-local repair:
  - `git config --local credential.interactive always`
  - `git config --local credential.https://dev.azure.com.useHttpPath true`
  - clear askpass env in current shell and set `GIT_TERMINAL_PROMPT=1`
- For Azure Repos with SSH git remote + LFS upload failures, set LFS HTTPS endpoint explicitly and disable locks verify for that endpoint.
