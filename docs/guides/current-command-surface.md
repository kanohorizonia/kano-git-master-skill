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

## Version

```bash
# Binary build version (short)
./scripts/kog version

# Full build metadata: branch, revision, hash, toolchain, preset, configuration, CI context
./scripts/kog version --verbose

# Workspace project version overview (reads VERSION files from discovered repos)
./scripts/kog version --workspace
./scripts/kog version --workspace --repo <path>
```

`kog version` reports the version string embedded in the native binary at build time.
`kog version --verbose` emits all build metadata fields in `key=value` format.
`kog version --workspace` is the project-version scanner for multi-repo workspaces.

`kog self status` reports install and checkout state (repo path, binary path,
installed version, install timestamp, packaged/developer checkout).

```bash
./scripts/kog self status
./scripts/kog self status --json
```

> **Note:** `kog self version` is deprecated. Use `kog version` for the binary
> build version, or `kog self status` for install/checkout state.
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
./scripts/kog auth doctor
./scripts/kog auth test --selected-remotes

# Product export
./scripts/kog export --help
./scripts/kog export --single
./scripts/kog export --single --validate-release-archive
./scripts/kog export --subtree "/path/to/repo/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head
./scripts/kog export upload doctor
./scripts/kog export upload --last
./scripts/kog export upload --archive .kano/tmp/git/export/<archive>.tar --target drive_sync

# Bash completion
./scripts/kog completion install bash
./scripts/kog completion uninstall bash
```

Unknown top-level commands now return a git-style error and suggest the most similar public commands.

`kog fetch` recursively discovers repositories and runs parallel `git fetch` with `--all --prune --tags` defaults. Use `--remote <name>` to target one remote, `--jobs/-j auto|N` for concurrency, and `--dry-run` to preview commands.

`kog auth doctor` inspects Git Credential Manager-facing configuration without storing tokens. It redacts credentials in any explicit `--url` input and reports the selected remote auth surface for the current repo or discovered workspace repos.

`kog auth test` runs a non-interactive `git ls-remote ... HEAD` probe against the selected remote set, a single configured remote, all local remotes, or an explicit URL. It does not write tokens into remotes, does not persist credentials, and treats file/local remotes as skipped rather than failed.

## Sync and converge dry-run preflight

`kog sync origin-latest --dry-run` and `kog converge --dry-run` are branch-preserving
preflight flows. `kog converge` remains backwards-compatible with the existing
repo-state convergence workflow; `kog converge repos` is the explicit taxonomy
alias for the same synced/committed/pushed repository-state behavior.

```bash
./scripts/kog converge --dry-run
./scripts/kog converge repos --dry-run
```

These repo-state preflight flows surface explicit blocker reason codes for unsafe
states, including active rebase/merge/cherry-pick/revert/bisect operations,
unmerged paths, detached HEAD, unresolved submodule/gitlink markers, fetch
failures, branch divergence, and unreachable gitlink commits.

`kog converge branches plan` is a read-only branch convergence planner. It uses
the registered recursive repo/submodule/worktree graph rather than arbitrary
filesystem recursion, reports child repositories before parent gitlink state,
defaults to a recorded `rebase` strategy, and records explicit `merge` or
`cherry-pick` overrides when requested. `kog converge branches inventory` and its `status`
alias emit the same branch/worktree divergence model as a read-only inventory
surface and exit successfully when blockers are present.

```bash
./scripts/kog converge branches plan --target main
./scripts/kog converge branches plan --target main --strategy merge --json
./scripts/kog converge branches plan --target main --strategy cherry-pick --json
./scripts/kog converge branches inventory --target main --json
```

The branch planner emits stable JSON with `--json` or in agent mode. It records
candidate branches/worktrees, dirty/unpushed/stale/active-worktree blockers,
merged status, proposed actions, traversal order, and `mutationPerformed=false`.
A stale target branch behind its upstream, a stale non-target local branch behind
its upstream, unpushed local commits, dirty worktree state, or a missing target
ref is reported as a blocker and makes the planner exit non-zero.

Branch mutations are explicit subcommands. `kog converge branches apply` requires
`--confirm`, fetches and fast-forwards the target branch by default, and then only
integrates non-blocked local branches. The default `rebase` strategy is currently
fail-closed fast-forward target advancement: it advances the target with
`git merge --ff-only` only when the target is already an ancestor of the branch.
`--strategy cherry-pick --branch <name>` replays missing, non-equivalent commits
from one selected source branch onto the target branch. `--strategy merge`
performs an explicit no-fast-forward merge. Successful apply paths push the target
branch and report machine-readable blockers instead of resolving conflicts.
Empty/no-op cherry-pick commits are skipped as already equivalent. Cherry-pick
conflicts stop for operator recovery; KOG does not auto-resolve them.

`kog converge branches retire` previews by default. With `--confirm`, it deletes
only local branches proven integrated into the target by merge-base ancestry,
patch equivalence, or an isolated empty/no-op cherry-pick proof. For proven
integrated branches, retire-specific policy treats removable clean worktree
leases, local-ahead markers, local-stale markers, and unrelated untracked-only
target dirt as cleanup metadata rather than hard data-loss blockers.
`--remove-worktrees` allows clean Git-managed worktrees for those branches to be
removed first, and `--delete-remote` additionally deletes the tracked upstream
branch when one is configured.

```bash
./scripts/kog converge branches apply --target main --confirm --json
./scripts/kog converge branches apply --target main --strategy cherry-pick --branch feature/example --confirm --json
./scripts/kog converge branches retire --target main --remove-worktrees --confirm --json
./scripts/kog converge branches retire --target main --remove-worktrees --delete-remote --confirm --json
```

Dry-run summaries are audit-first and must not be treated as clean when blockers exist.
If blockers are reported, resolve them first or run explicit Kano repair flows
(`kog sync pre-commit`, `kog sync origin-latest`, `kog converge`) after reviewing
the dry-run plan.

`kog sync origin-latest`, `kog sync dev`, and default `kog sync` now run a Git Credential Manager-focused auth preflight against the selected HTTP(S) remote before fetch/rebase work begins. Use `--no-auth-preflight` to skip that check when you explicitly need old behavior.

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
./scripts/kog export --subtree "/path/to/repo/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head
./scripts/kog export --subtree Engine/Source/Programs/UnrealGameSync --source working-tree
```

Subtree mode compatibility:
- `--subtree` accepts absolute and relative paths.
- `--subtree` is standalone export mode and cannot be combined with `--single` or `--include-submodule-stubs`.
- `--subtree` skips release archive validation; explicit `--validate-release-archive` is rejected.
- Use `--keep-subtree-path` if archive entries should retain full repo-relative subtree path.

## Export upload

`kog export upload` uploads or copies an existing export archive after `kog export`.
It supports a local sync folder backend and an rclone backend. The Google Drive
API backend is guidance-only for now; `doctor` does not start OAuth or store
tokens.

```bash
./scripts/kog export upload doctor
./scripts/kog export upload doctor --target gdrive-api
./scripts/kog export upload --last
./scripts/kog export upload --archive .kano/tmp/git/export/<archive>.tar --target drive_sync
./scripts/kog export upload --last --target drive_sync --layout Kano/kog --copy-manifest --copy-sha256
```

Configure upload targets in layered TOML config files:

```text
user: ~/.kano/kog_config.toml
repo: .kano/kog_config.toml
precedence: user < repo < CLI
```

```toml
[export.upload]
default_target = "drive_sync"

[export.upload.targets.drive_sync]
type = "local-sync-folder"
path = "/path/to/sync/root"
layout = "Kano/kog"
copy_manifest = true
copy_sha256 = true
return_url = false

[export.upload.targets.gdrive]
type = "rclone"
remote = "kog-drive"
destination = "exports/kog"
layout = "ChatGPT_Export"
copy_manifest = true
copy_sha256 = true
return_url = true
```

Target behavior:
- `local-sync-folder` must be an existing sync root, for example a Google
  Drive/Desktop folder such as `/path/to/sync/root`. KOG creates only
  the safe relative `layout` directories under that root, copies the archive,
  and leaves cloud propagation to the sync client. It never fabricates a cloud
  URL for ChatGPT's Google connector.
- `rclone` uses `copyto` against an existing configured remote and destination.
  The optional original export manifest and `.sha256` sidecar are uploaded only
  when enabled by `copy_manifest` / `copy_sha256` or CLI flags. KOG does not
  install rclone, start Google OAuth, or store tokens.
- For Google Drive remotes, KOG derives a private Drive URL only when
  `rclone lsjson --stat -M` returns a file ID. If no ID is available, the upload
  manifest records `URL_UNAVAILABLE` instead of pretending a URL exists.

Upload safety rules:
- Uploads preserve the target backend's existing private/default visibility.
- `rclone link` is not called by default.
- Public links require explicit CLI confirmation: `--public-link --yes`.
- Config-file `public_link` or `yes` values do not enable permission mutation.
- Treat `--public-link --yes` as a sharing-permission change; it may create or
  retrieve a public link through rclone.
- rclone targets must name an existing configured remote; inline credentials or
  credential-looking remote/destination strings are rejected before invoking rclone.

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

Codex app-server diagnostic/thread utilities live under
`src/shell/codex-appserver/`. They are support scripts for local observer and
session debugging, not primary Git Master workflows. Ark-owned runner/provider
helpers such as `codex-appserver-send.ps1` stay in Ark until KOA-TSK-0223
productionizes that path.
