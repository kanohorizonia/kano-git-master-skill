# Current Command Surface

This document is the current public entrypoint contract for `kano-git-master-skill`.
Prefer these commands over older shell examples in historical design notes.

## Canonical launchers

Use the repo-local launchers from the repository root:

```bash
./scripts/kog --help
./scripts/kano-git --help
```

On Windows CMD or PowerShell, use the paired batch launchers:

```bat
scripts\kog.bat --help
scripts\kano-git.bat --help
```

The `kog` and `kano-git` launchers target the same native CLI surface. The shorter
`kog` name is preferred for interactive work and agent workflows.

## Native build

The normal developer build may fetch C++ dependencies from GitHub through CMake
FetchContent. This is expected for online developer and CI environments.

```bash
./scripts/kog self build
```

Use a full rebuild when the native output or CMake cache must be discarded:

```bash
./scripts/kog self rebuild
```

If dependency fetching fails in an offline or DNS-restricted environment, treat it
as an online build prerequisite failure, not as a launcher failure.

## Local quality gate

Run the shell-only quality gate before committing or preparing an archive:

```bash
src/shell/test/pre-commit-quality-gate.sh
```

Enable the tracked pre-commit hook for this repository with:

```bash
git config core.hooksPath .githooks
```

## Offline release smoke test

The archive smoke test must not require network access. It checks launcher
presence, executable bits, shell syntax, fallback help, and broken submodule
`.git` pointer files.

```bash
src/shell/test/smoke-release-archive.sh <archive.tar>
```

## Online release build smoke test

The online build smoke test intentionally runs the native build and may require
GitHub access for FetchContent dependencies.

```bash
src/shell/test/smoke-release-online-build.sh <archive.tar>
```

## Common workflows

```bash
# Workspace overview
./scripts/kog status
./scripts/kog overview
./scripts/kog discover
./scripts/kog fetch
./scripts/kog log --remote-count 3

# AI/plan-assisted commit flows
./scripts/kog plan new
./scripts/kog plan runbook commit
./scripts/kog ai bootstrap copilot --dry-run
./scripts/kog ai bootstrap copilot
./scripts/kog commit -m "chore: update workspace"
./scripts/kog commit-push -m "chore: update workspace"
./scripts/kog cpa

# Repo hygiene
./scripts/kog repo-hygiene check
./scripts/kog repo-hygiene fix

# Product export
./scripts/kog export --help
./scripts/kog export --single
./scripts/kog export --single --validate-release-archive
./scripts/kog export --subtree "E:/_gamedev/KanoTamaoProject/UnrealEngine/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head

# Bash completion
./scripts/kog completion install bash
./scripts/kog completion uninstall bash
```

Unknown top-level commands now return a git-style error and suggest the most similar public commands.

`kog fetch` recursively discovers repositories and runs parallel `git fetch` with `--all --prune --tags` defaults. Use `--remote <name>` to target one remote, `--jobs/-j auto|N` for concurrency, and `--dry-run` to preview commands.

## Sync and converge dry-run preflight

`kog sync origin-latest --dry-run` and `kog converge --dry-run` are branch-preserving
preflight flows. They now surface explicit blocker reason codes for unsafe states,
including active rebase/merge/cherry-pick/revert/bisect operations, unmerged paths,
detached HEAD, unresolved submodule/gitlink markers, fetch failures, branch divergence,
and unreachable gitlink commits.

Dry-run summaries are audit-first and must not be treated as clean when blockers exist.
If blockers are reported, resolve them first or run explicit Kano repair flows
(`kog sync pre-commit`, `kog sync origin-latest`, `kog converge`) after reviewing
the dry-run plan.

Do not treat raw `git submodule update` as the default repair path for these cases.

`kog log` and `kog slog` now support behind/diverged remote preview controls:
- `--remote-count <N>` limits remote-only preview lines.
- `--no-remote-preview` disables remote-only preview lines.
- Layered config key: `[log] remote_preview_count = 3`.

AI model selection keywords:
- `auto`: provider-native auto first, fallback to KOG auto if unsupported.
- `provider-auto`: force provider-native auto mode.
- `kog-auto`: force KOG change-count policy.
- `provider-default`: omit provider model argument and use provider default.

Copilot bootstrap scope:
- Automatic bootstrap command support is Windows/WinGet only in this release.
- Linux/macOS are detection-only/manual setup paths for Copilot CLI.

## Export release validation

`kog export` keeps the normal CMake/GitHub dependency model unchanged. Archive
validation is an offline release check, not an online build check.

For single-file release archives, use:

```bash
./scripts/kog export --single
```

When the workspace contains `src/shell/test/smoke-release-archive.sh`, `kog
export --single` automatically runs that smoke test against the root `.tar`
archive. The default multi-archive mode skips this validation because the root
archive does not contain submodule working-tree contents.

Force validation and fail if it cannot run:

```bash
./scripts/kog export --single --validate-release-archive
```

Disable validation explicitly:

```bash
./scripts/kog export --single --no-validate-release-archive
```

Subtree-style standalone export (strip parent path by default):

```bash
./scripts/kog export --subtree "E:/_gamedev/KanoTamaoProject/UnrealEngine/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head
./scripts/kog export --subtree Engine/Source/Programs/UnrealGameSync --source working-tree
```

Subtree mode compatibility:
- `--subtree` accepts absolute and relative paths.
- `--subtree` is standalone export mode and cannot be combined with `--single` or `--include-submodule-stubs`.
- `--subtree` skips release archive validation; explicit `--validate-release-archive` is rejected.
- Use `--keep-subtree-path` if archive entries should retain full repo-relative subtree path.

## Shell script policy

The root `scripts/` directory is intentionally small:

```text
scripts/kog
scripts/kano-git
scripts/kog.bat
scripts/kano-git.bat
scripts/setup-global-tools.sh
```

Do not document new primary workflows as old root shell paths such as `scripts/core/*`, `scripts/internal/*`, `scripts/submodules/*`, `scripts/self/*`, `scripts/stages/*`, or `scripts/workflows/*` unless those paths are restored and covered by release smoke tests.

Shared build helpers live under `src/cpp/shared/infra/scripts/`. They are backing
infrastructure for native build/test flows, not the main user-facing Git command
surface.
