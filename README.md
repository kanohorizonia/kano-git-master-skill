# Git Master Skill

**Version**: 0.1.0-beta  
**Status**: Beta Release

Advanced Git automation for multi-repository workspaces with a native `kog` /
`kano-git` command surface.

## Quick Start

Use the repo-local launcher first. It works even before the native binary is
built and prints the current launcher-level command surface.

```bash
./scripts/kog --help
```

Build the native CLI when you need the full command surface:

```bash
./scripts/kog self build
```

The normal developer build may fetch C++ dependencies from GitHub through CMake
FetchContent. That is expected for online developer and CI builds.

## Current Command Surface

Start here when updating docs, writing agent prompts, or validating a release:

- [Current Command Surface](./docs/guides/current-command-surface.md)
- [Documentation Index](./docs/README.md)
- [CPA Commit Plan Workflow](./docs/guides/cpa-commit-plan-workflow.md)
- [Repo Hygiene](./docs/repo-hygiene.md)

Common commands:

```bash
# Help / discovery
./scripts/kog --help
./scripts/kog status
./scripts/kog overview
./scripts/kog discover

# Commit / plan flows
./scripts/kog plan new
./scripts/kog plan runbook commit
./scripts/kog commit -m "chore: update workspace"
./scripts/kog commit-push -m "chore: update workspace"
./scripts/kog cpa

# Hygiene / export
./scripts/kog repo-hygiene check
./scripts/kog repo-hygiene fix
./scripts/kog export --help
./scripts/kog export --single
./scripts/kog export --subtree "E:/_gamedev/KanoTamaoProject/UnrealEngine/Engine/Source/Programs/UnrealGameSync" --name UnrealGameSync --source head
./scripts/kog export --subtree Engine/Source/Programs/UnrealGameSync --source working-tree
```

Subtree standalone export notes:
- `--subtree` accepts absolute or relative paths.
- Default archive root strips parent path (`UnrealGameSync/...`).
- Use `--keep-subtree-path` to keep full repo-relative path in archive.
- `--subtree` cannot be combined with `--single` or `--include-submodule-stubs`.
- `--subtree` skips release archive smoke validation; `--validate-release-archive` is rejected.

## Wrapper Entry Points

- `scripts/kog` and `scripts/kano-git` are the canonical Unix launchers.
- `scripts/kog.bat` and `scripts/kano-git.bat` are the Windows CMD/PowerShell
  launchers.
- Do not use separate root installer wrappers such as `scripts/kog-installer` and `scripts/kano-git-installer`; they are not part of the current command surface.
- Bash completion is installed through the native command surface:

```bash
./scripts/kog completion install bash
```

## Build and Test

Preferred local quality gate before committing:

```bash
src/shell/test/pre-commit-quality-gate.sh
```

Enable the tracked Git hook when working on this repository:

```bash
git config core.hooksPath .githooks
```

Preferred native build:

```bash
./scripts/kog self build
```

Offline archive smoke test:

```bash
src/shell/test/smoke-release-archive.sh <archive.tar>
```

Online build smoke test:

```bash
src/shell/test/smoke-release-online-build.sh <archive.tar>
```

Single-file release export automatically runs the offline archive smoke test
when the smoke script is present:

```bash
./scripts/kog export --single
./scripts/kog export --single --validate-release-archive
```

Shared native build/test/report/bootstrap helpers live in
`src/cpp/shared/infra/scripts/`. Use them as infrastructure backing scripts, not
as the primary Git workflow UX.

## Documentation Status

The native C++ CLI is now the source of truth for current workflows. Some older
architecture notes and historical examples may still mention retired root shell
scripts. Treat those as legacy notes unless they are referenced from
[Current Command Surface](./docs/guides/current-command-surface.md).
