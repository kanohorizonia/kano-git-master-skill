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

### Launcher diagnostics

Successful Pixi bootstrap messages, binary candidate probes, and native scoped
timing logs are quiet by default. Errors, warnings, build progress, and command
results remain visible. Use the existing `KOG_DEBUG` contract when those
diagnostics are needed:

```bash
KOG_DEBUG=1 ./scripts/kog --help
```

Accepted truthful values are `1`, `true`, `yes`, and `on`, case-insensitive.
Because `KOG_DEBUG` also enables existing command-layer debug output, leave it
unset for ordinary and machine-readable workflows.

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

Provider images can build the cacheable minimal runtime bundle without test or
TUI artifacts:

```bash
cmake --build <build-dir> --target kog_runtime_artifact
```

The target writes `runtime-artifact/bin/kano-git`, the KOG-owned shared-library
closure plus required GNU runtime libraries on Linux, and
`runtime-artifact/manifest.json` with revision, source fingerprint, toolchain,
and build-context provenance. Linux packaging fails when `ldd` reports an
unresolved dependency. The root `.dockerignore` exposes only the VERSION file
and native runtime source projection to provider-image build contexts; local
outputs, reports, docs, tests, and workspace metadata do not invalidate that
Docker context.

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

### Nested repository ordering

Mutating `sync`, `commit`, and `commit-push` workflows execute registered
nested repositories in child-first dependency waves. A sync may prefetch parent
remote refs before those waves to discover current `.gitmodules` policy; that
prefetch does not mutate the parent working tree. Native plan lines expose the
active contract as `order=child-first` and report the wave count.

Recursive `kog sync`, `kog sync origin-latest`, and `kog sync dev` accept
`--execution-policy serial|parallel`. `parallel` is the default and runs
independent repositories concurrently up to `--jobs`; `serial` keeps the same
deterministic dependency waves but executes one repository at a time as a
fallback and debugging mode. Plan output records the selected policy and the
effective worker count.

```bash
# Workspace overview
./scripts/kog status
./scripts/kog status .
./scripts/kog status path/to/repo --all --format json
./scripts/kog repo status path/to/repo --format json
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
./scripts/kog commit --exact-path src/file.cpp -m "fix: update one file" --dry-run
./scripts/kog commit-push -m "chore: update workspace"
./scripts/kog agent-queue status
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

`kog status <target>` resolves a repository name or path and uses that repository
as the discovery root, so registered repositories below it remain part of the
result. Use `.` for the current repository. If a name matches more than one
repository, KOG rejects the request and prints the candidates in deterministic
path order.

`kog repo` is the native single-repository namespace. `kog repo status <target>`
reports exactly the resolved repository without recursively expanding registered
children. The same namespace exposes scoped `log`, `slog`, `push`, `commit`,
`commit-push`, and `update` variants; run `kog repo --help` for their arguments.

Unknown top-level commands now return a git-style error and suggest the most similar public commands.

## Shared-checkout agent queue and exact commits

`kog agent-queue` coordinates low-conflict coding-agent mutations in one shared
checkout. State lives under the Git common directory, so all worktrees of one
repository observe the same pending and active batch. Queue mutations use a
KOG-owned lock and atomic state replacement; KOG reports an existing lock and
does not delete it.

```bash
./scripts/kog agent-queue admit --work-item KG-123 --agent codex-1 --file src/a.cpp --chunk src/a.cpp:10-30 --validate "pixi run quick-test"
./scripts/kog agent-queue drain
./scripts/kog agent-queue drain --confirm
./scripts/kog commit --exact-path src/a.cpp --queue-batch <batch-id> -m "[Git][BugFix] Update A (KG-123)"
./scripts/kog agent-queue complete --batch <batch-id> --status succeeded
```

Drain merges disjoint files, identical declared `path=value` postconditions, or
same-file chunks whose inclusive line ranges do not overlap. Unspecified or
overlapping same-file ownership fails closed without consuming pending items.
Stale base HEAD also blocks drain.

`kog commit --exact-path` creates a temporary index from HEAD, stages only the
listed paths, runs normal Git commit hooks against that isolated index, and then
reconciles only those paths in the shared index. Unrelated staged entries remain
unchanged. `--dry-run` reports `included` and `excluded` paths. Selectors outside
the repository, directories, overlapping selectors, stale `--expected-head`,
active-batch scope mismatches, and an existing `index.lock` are blockers. For a
rename, list both old and new paths. See `agent-mutation-queue.md` for the full
agent policy.

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
When a source branch was already integrated through reviewed conflict resolution
or a superseding implementation, `--record-reviewed-integration` provides an
explicit ancestry closure instead of replaying stale commits. It is restricted
to `--no-recursive`, requires one branch, its exact 40-character source HEAD, a
review reason, a marker commit message, and `--confirm`. KOG requires clean target
and source worktrees, creates an `ours` ancestry marker, verifies that the target
tree hash is unchanged, and pushes the target. A source HEAD mismatch fails closed.

Recursive branch planning keeps the bounded 5-second probe and 90-second plan
defaults. Exact-repository `--no-recursive` planning uses 30-second probes and a
240-second plan deadline because conflict/no-op probes can legitimately exceed
the recursive discovery budget. `KOG_BRANCH_PROBE_TIMEOUT_MS` and
`KOG_BRANCH_PLAN_DEADLINE_MS` still override these defaults.

`kog converge branches retire` previews by default. With `--confirm`, it deletes
only local branches proven integrated into the target by merge-base ancestry,
patch equivalence, or an isolated empty/no-op cherry-pick proof. For proven
integrated branches, retire-specific policy treats removable clean worktree
leases, local-ahead markers, local-stale markers, and unrelated untracked-only
target dirt as cleanup metadata rather than hard data-loss blockers.
`--remove-worktrees` allows clean Git-managed worktrees for those branches to be
removed first, and `--delete-remote` additionally deletes the tracked upstream
branch when one is configured.
`--prune-worktrees` prunes stale Git worktree metadata. `--harvest-detached-worktrees`
is narrower: with `--remove-worktrees` and `--confirm`, it only harvests dirty
detached worktrees whose HEAD is proven integrated into the target, applies the
exact binary patch to the target, commits and pushes it, then removes the
detached worktree. Dirty detached non-ancestor worktrees, primary worktrees, and
unsafe dirty targets remain blockers.
`--harvest-branch-worktrees` applies the same guarded binary-patch flow to dirty
attached worktrees only when their branch HEAD is proven integrated. Inventory
and settle preflight report each worktree's changed paths plus a branch-scoped
recovery command. `--branch <name>` limits retire/harvest to one branch. Dirty
paths shared by multiple worktrees are blocked as `DIRTY_WORKTREE_OVERLAP` and
must be resolved before mutation. Dirty worktrees on branches that are not yet
proven integrated remain blocked as `DIRTY_BRANCH_WORKTREE_NOT_INTEGRATED` with
their changed paths and the branch integration command.
Top-level `kog converge --settle-worktrees` inserts this guarded retire/prune
step before the final status summary when explicitly requested; ordinary
`kog converge` keeps the default phase list unchanged.

```bash
./scripts/kog converge branches apply --target main --confirm --json
./scripts/kog converge branches apply --target main --strategy cherry-pick --branch feature/example --confirm --json
./scripts/kog converge branches apply --no-recursive --target main --branch feature/reviewed --record-reviewed-integration --expected-source-head <40-char-sha> --review-reason "integrated with reviewed conflict resolution" --marker-message "[Converge][Chore] Record reviewed integration (KG-BUG-0012)" --confirm --json
./scripts/kog converge branches retire --target main --remove-worktrees --confirm --json
./scripts/kog converge branches retire --target main --remove-worktrees --delete-remote --confirm --json
./scripts/kog converge branches retire --target main --branch feature/example --remove-worktrees --harvest-branch-worktrees --confirm --json
./scripts/kog converge branches retire --target main --remove-worktrees --harvest-detached-worktrees --prune-worktrees --confirm --json
./scripts/kog converge --settle-worktrees --target main --remove-worktrees --harvest-branch-worktrees --harvest-detached-worktrees --prune-worktrees
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
