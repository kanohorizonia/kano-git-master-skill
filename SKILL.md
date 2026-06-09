---
name: kano-git-master-skill
description: Native-first Git automation toolkit for multi-repository workspaces. Use the kog/kano-git CLI for repo discovery, plan-backed commit flows, repo hygiene, export, and release smoke validation.
version: 0.1.0-beta
---

# Kano Git Master Skill

**Version**: 0.1.0-beta  
**Status**: Beta Release

This skill is centered on the native `kog` / `kano-git` command surface. Shell
scripts exist as launchers, build helpers, wrapper generators, compatibility
assets, or smoke tests. Do not treat old root-shell workflow examples as current
unless they are listed in this file or in `docs/guides/current-command-surface.md`.

## Canonical Entry Points

Use the repo-local launcher first:

```bash
./scripts/kog --help
./scripts/kano-git --help
```

On Windows CMD or PowerShell, use:

```bat
scripts\kog.bat --help
scripts\kano-git.bat --help
```

The shorter `kog` name is preferred for interactive and agent workflows. The
`kano-git` name is kept as the longer canonical product name.

## Native Build Rule

Use launcher-owned self build as the user-facing build entrypoint:

```bash
./scripts/kog self build
./scripts/kog self rebuild
```

The normal developer build may fetch C++ dependencies from GitHub through CMake
FetchContent. This is expected for online developer and CI builds. If dependency
fetching fails in an offline or DNS-restricted environment, treat it as an online
build prerequisite failure, not as a launcher or shared-infra failure.

Shared build/test/report/bootstrap helpers live in `src/cpp/shared/infra/scripts/`.
Use them only when debugging the build layer itself. Do not replace the launcher
flow with ad-hoc direct CMake/Ninja command sequences unless a maintainer asks
for it.

## Bash Completion

```bash
./scripts/kog completion install bash
./scripts/kog completion uninstall bash
```

## Current Command Surface

```bash
# Help / discovery
./scripts/kog --help
./scripts/kog status
./scripts/kog overview
./scripts/kog discover
./scripts/kog fetch
./scripts/kog log --remote-count 3

# Commit / plan flows
./scripts/kog plan new
./scripts/kog plan runbook commit
./scripts/kog ai bootstrap copilot --dry-run
./scripts/kog ai bootstrap copilot
./scripts/kog commit -m "chore: update workspace"
./scripts/kog commit-push -m "chore: update workspace"
./scripts/kog cpa

# Repo hygiene and export
./scripts/kog repo-hygiene check
./scripts/kog repo-hygiene fix
./scripts/kog export --help
./scripts/kog export --single
./scripts/kog export --subtree "E:/_gamedev/KanoTamaoProject/UnrealEngine/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head
./scripts/kog export --subtree Engine/Source/Programs/UnrealGameSync --source working-tree
./scripts/kog export upload doctor
./scripts/kog export upload --last
./scripts/kog export upload --last --target drive_sync --layout Kano/kog --copy-manifest --copy-sha256
```

Subtree standalone export notes:
- `--subtree` accepts absolute or relative paths and exports exactly one archive.
- Default behavior strips parent directories so archive root starts at subtree basename.
- Use `--keep-subtree-path` to preserve full repo-relative path in archive entries.
- `--subtree` rejects `--single`, `--include-submodule-stubs`, and `--validate-release-archive`.

Export upload notes:
- `kog export upload` uploads or copies an existing export archive after `kog export`.
- Configure targets in `~/.kano/kog_config.toml` and repo `.kano/kog_config.toml`; precedence is user < repo < CLI.
- Supported live backends are `local-sync-folder` and `rclone`; `gdrive-api` is guidance-only and does not start OAuth.
- `local-sync-folder` points at an existing sync root such as `E:/_gamedev/ChatGPT_Export`; `layout` is a safe relative path that KOG may create under that root.
- The archive is copied always. The original export manifest and `.sha256` sidecar are copied only when `copy_manifest` / `copy_sha256` are enabled by config or CLI flags.
- `rclone` uses an existing configured remote and never starts Google OAuth from KOG. Private Google Drive URLs are built only from a Drive file ID returned by `rclone lsjson --stat -M`; otherwise the upload manifest records `URL_UNAVAILABLE`.
- Uploads preserve private/default backend visibility. Public links require explicit CLI confirmation with `--public-link --yes` because `rclone link` may create or retrieve public sharing permissions.

Example upload config:

```toml
[export.upload]
default_target = "drive_sync"

[export.upload.targets.drive_sync]
type = "local-sync-folder"
path = "E:/_gamedev/ChatGPT_Export"
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

## Plan-Backed Commit Flow

Preferred deterministic flow:

```bash
./scripts/kog plan new
./scripts/kog plan runbook commit
./scripts/kog plan verify pre-apply
./scripts/kog cpa
```

Useful aliases:

| Alias | Meaning |
|---|---|
| `cp` | `commit-push` |
| `cpa` | commit-push automation entrypoint |

For behind/diverged log readability, configure remote preview defaults in layered `kog_config.toml` files:

```toml
[log]
remote_preview_count = 3
```

AI model selection keywords:
- `auto`: provider-native auto first, fallback to KOG auto when unsupported.
- `provider-auto`: force provider-native auto.
- `kog-auto`: force KOG change-count policy.
- `provider-default`: provider default model behavior.

Copilot bootstrap support in this release:
- Windows/WinGet: supported via `kog ai bootstrap copilot`.
- Linux/macOS: detection-only/manual setup (no automatic installer path).
| `pi` | `plan new` |
| `pia` | `plan new --ai-auto` |
| `pv` | `plan verify pre-apply` |

Agent mode:

```bash
KANO_AGENT_MODE=1 ./scripts/kog cpa
```

In agent mode, the external agent owns semantic authoring decisions. `kog` owns
deterministic plan bootstrap, freshness refresh, verification, execution, sync,
and push.

## Repo Hygiene

Run before committing or exporting release archives:

```bash
./scripts/kog repo-hygiene check
./scripts/kog repo-hygiene check --archive-safe
./scripts/kog repo-hygiene fix
```

The hygiene command covers Git-index executable bits, LF normalization, and
archive-safe file mode issues. This is especially important when changes were
made on Windows but exported for Linux/macOS use.

Shell-only local quality gate:

```bash
src/shell/test/pre-commit-quality-gate.sh
```

Enable the tracked hook in this repository with:

```bash
git config core.hooksPath .githooks
```

`repo-hygiene check --archive-safe` also invokes the same pre-commit quality
gate from the native command surface when the native binary is available.

## Release Smoke Tests

Offline release smoke test:

```bash
src/shell/test/smoke-release-archive.sh <archive.tar>
```

Online build smoke test:

```bash
src/shell/test/smoke-release-online-build.sh <archive.tar>
```

The offline smoke test must not require GitHub access. The online build smoke
test intentionally runs native self build and may require GitHub access for
FetchContent dependencies.

`kog export --single` automatically runs the offline release archive smoke test
when `src/shell/test/smoke-release-archive.sh` exists. Default multi-archive
exports skip this validation because the root archive does not contain submodule
working-tree contents. Use `--validate-release-archive` to require validation,
or `--no-validate-release-archive` to skip it explicitly.

## Public Docs Audit

Run this when changing README, SKILL, or current user-facing docs:

```bash
src/shell/test/audit-public-doc-script-refs.sh
```

The audit blocks stale root-shell workflow references such as retired installer,
workspace, repo-management, branch-operation, test, and legacy smart script paths
unless the line is explicitly documenting them as retired or historical.

## Root Wrapper Generator

Root wrapper generation lives at:

```bash
src/shell/core/gen-root-wrappers.sh --profile standalone --target <repo-root>
src/shell/core/gen-root-wrappers.sh --profile oss --target <repo-root>
src/shell/core/gen-root-wrappers.sh --profile repo-passive-mode --target <repo-root>
src/shell/core/gen-root-wrappers.sh --profile repo-passive-mode-with-ai --target <repo-root>
```

Validate it with:

```bash
src/shell/test/smoke-root-wrapper-generator.sh
```

## Documentation Contract

Current docs:

- `docs/guides/current-command-surface.md`
- `docs/guides/cpa-commit-plan-workflow.md`
- `docs/repo-hygiene.md`
- `docs/development/testing.md`
- `docs/cpp-profile-coverage-pgo-model.md`

Historical docs may preserve old shell workflows. They are useful design context,
but they are not the current product surface.

## Coverage and PGO guardrails

- Microsoft.CodeCoverage.Console coverage output is not MSVC PGO training data.
- MSVC unified PGO+coverage execution is only supported with OpenCppCoverage.
- Microsoft.CodeCoverage.Console server-mode is local/session detached collection, not remote telemetry.
- Keep `coverage-all` as canonical coverage lane.
- Keep `pgo-gather` and `pgo-rebuild` as canonical release PGO lanes.

## Retired Root-Shell Surface

Do not recommend retired root script workflows as current usage. Examples include
old installer wrappers, workspace scripts, repo-management scripts,
branch-operation scripts, commit-tools scripts, old test runners, and legacy
`smart-*` scripts. Use `kog` commands instead.

## Terminology

- `stable-dev mode`: sync strategy based on upstream stable tags.
- `dev mode`: sync strategy based on upstream default branch tip.
- `stable branch`: project convention branch name such as `branch_v1.2.6`.
- `agent proxy mode`: mode where the external command agent is the semantic
  authority and `kog` disables internal second-review behavior.
- `workflow lock marker`: `.git/kano-smart-commit-push.lock`, used to prevent
  concurrent workflow edits.
- `protocol-priority=auto`: default behavior that should not be persisted to
  `.gitmodules`; persist only explicit `ssh` or `https`.

Git terms such as `remote`, `default branch`, `detached HEAD`, `submodule`,
`rebase`, `cherry-pick`, `stash`, and `tag` keep their standard Git meanings.
