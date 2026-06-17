# Git Master Skill Quick Start

This guide uses the current native `kog` command surface. Historical root shell
scripts such as `update-repo.sh`, `discover-repos.sh`, and
`status-all-repos.sh` are no longer the supported entrypoints.

CLI naming:
- `kano-git` is the full command name.
- `kog` is the short alias used by examples.

## First Check

From the repository root:

```bash
./scripts/kog --help
./scripts/kog self status
./scripts/kog status
```

On Windows PowerShell or CMD:

```bat
scripts\kog.bat --help
scripts\kog.bat self status
scripts\kog.bat status
```

If the native binary is missing or stale, build through the launcher:

```bash
./scripts/kog self build
```

## Common Workflows

### Inspect Workspace

```bash
./scripts/kog overview
./scripts/kog discover
./scripts/kog fetch
./scripts/kog log --remote-count 3
```

### Plan-Backed Commit

```bash
./scripts/kog plan new
./scripts/kog plan runbook commit
./scripts/kog plan verify pre-apply
./scripts/kog commit-push -m "chore: update workspace"
```

For the agent-mode shortcut:

```bash
KANO_AGENT_MODE=1 ./scripts/kog cpa
```

### Repo Hygiene

```bash
./scripts/kog repo-hygiene check
./scripts/kog repo-hygiene check --archive-safe
./scripts/kog repo-hygiene fix
```

### Export

```bash
./scripts/kog export --help
./scripts/kog export --single
./scripts/kog export --single --validate-release-archive
./scripts/kog export upload doctor
./scripts/kog export upload --last
```

Subtree export accepts absolute or repository-relative paths:

```bash
./scripts/kog export --subtree Engine/Source/Programs/UnrealGameSync --source working-tree
```

## Command Map

| Need | Current command |
| --- | --- |
| Show command surface | `./scripts/kog --help` |
| Check self build status | `./scripts/kog self status` |
| Build native binary | `./scripts/kog self build` |
| Show workspace status | `./scripts/kog status` |
| Show workspace overview | `./scripts/kog overview` |
| Discover repos | `./scripts/kog discover` |
| Fetch remotes | `./scripts/kog fetch` |
| Show recent log | `./scripts/kog log --remote-count 3` |
| Create commit plan | `./scripts/kog plan new` |
| Verify plan | `./scripts/kog plan verify pre-apply` |
| Commit and push | `./scripts/kog commit-push -m "message"` |
| Agent commit/push automation | `KANO_AGENT_MODE=1 ./scripts/kog cpa` |
| Check hygiene | `./scripts/kog repo-hygiene check` |
| Export archive | `./scripts/kog export --single` |

## Related Guides

- [Current Command Surface](./current-command-surface.md)
- [CPA Commit Plan Workflow](./cpa-commit-plan-workflow.md)
- [Ignore Plan Operator Workflow](./ignore-plan-operator-workflow.md)
- [Ignore Datasource Sync Policy](./ignore-datasource-sync-policy.md)
- [Testing](../development/testing.md)
