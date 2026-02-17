# Ops Policies (Recent Learnings)

This reference captures operational policies that were validated during recent iterations.

## 1) Stable-Dev Sync Policy

### Precondition
- Working tree must be clean.
- Script auto-switches to target stable branch `branch_<target-tag>`.
- If target branch does not exist, create it from the target stable tag.

### `.gitmodules` branch bookkeeping
- During stable-dev sync, update superproject `.gitmodules` branch entry for target submodule branch.
- This keeps detached-head recovery aligned with intended maintenance branch.

### Wrapper scope
- `smart-sync-upstream-stable-dev.sh` (without `--repo`) scans only `src/*` submodules.
- Only repos with `upstream` remote are processed.
- Repos without `upstream` are skipped.

### End-of-run reporting
- Report is aggregated at bottom (single place to review all repos).
- Supported formats: `compact` (default), `table`, `tsv`, `json`, `markdown`.
- Commit fields: `sha | commit-time | author | title`.

## 2) Multi-Remote Push Policy

### Remotes attempted
- Push attempts all configured origin remotes:
  - `origin-ssh`
  - `origin-http`
  - `origin`

### Success condition
- Repo push is successful when at least one remote push succeeds.
- Repo fails only when all candidate origin remotes fail.
- Non-verbose mode suppresses partial-remote failure noise.

## 3) `kog-protocol-priority` Persistence Policy

- Default `auto` should not be persisted to `.gitmodules`.
- Persist only when user explicitly selects non-default value (`ssh` or `https`).

## 4) AI Review Gate Policy

### `smart-commit` gate behavior
- Explicit `FAIL` blocks commit.
- Empty/invalid review verdicts are warning-only (fail-open).
- Parser tolerates common small-model drift (for example `PASS - ...`, leading spaces).

### Cost-control shortcuts
- `smart-commit-push` supports `-noai` as shortcut for `--no-ai-review`.
- Delegated runs (`--agent <name>`, non-`manual`) auto-disable review and require fixed `-m/--message`.
