# Git Master Skill - Quick Reference

One-page reference for all Git Master Skill scripts.

CLI naming note:
- `kano-git` is the full command name.
- `kog` is the short alias of `kano-git`.

## One-Liners

```bash
# Update repo + registered subrepos
./scripts/update-repo.sh

# Clone fork with upstream
./scripts/clone-with-upstream.sh <fork-url> <upstream-url>

# Rebase to upstream
./scripts/rebase-to-upstream-latest.sh

# Discover all repos
./scripts/discover-repos.sh

# Update all repos
./scripts/update-workspace-repos.sh

# Check status
./scripts/status-all-repos.sh

# Run command in all repos
./scripts/foreach-repo.sh "git status"
```

## Script Comparison

| Script | Purpose | Scope | Key Feature |
|--------|---------|-------|-------------|
| `update-repo.sh` | Update single repo | 1 repo + registered subrepos | Fast, simple |
| `clone-with-upstream.sh` | Clone with upstream | 1 repo | Fork workflow |
| `rebase-to-upstream-latest.sh` | Rebase to upstream | 1 repo + registered subrepos | Sync with upstream |
| `discover-repos.sh` | Find all repos | Workspace | Discovery |
| `update-workspace-repos.sh` | Update multiple repos | Workspace | Batch update |
| `foreach-repo.sh` | Run commands | Workspace | Custom commands |
| `status-all-repos.sh` | Status report | Workspace | Monitoring |

## Common Options

All scripts support:
- `--help` - Show help
- `--dry-run` - Preview mode

Most scripts support:
- `--remote <name>` - Remote name (default: origin)
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type (root,registered,unregistered)
- `--exclude <pattern>` - Exclude patterns
- `--continue-on-error` - Don't stop on failures

## Quick Workflows

### Daily Sync
```bash
./scripts/status-all-repos.sh && \
./scripts/update-workspace-repos.sh && \
./scripts/foreach-repo.sh "git status --short"
```

### Fork Contribution
```bash
# Setup
./scripts/clone-with-upstream.sh <fork> <upstream>

# Sync
./scripts/rebase-to-upstream-latest.sh
```

### Batch Operations
```bash
# Discover
./scripts/discover-repos.sh --save manifest.json

# Update
./scripts/update-workspace-repos.sh --manifest manifest.json

# Status
./scripts/status-all-repos.sh --manifest manifest.json
```

## Output Formats

### discover-repos.sh
- `--format list` (default) - Human-readable
- `--format json` - Machine-readable

### status-all-repos.sh
- `--format table` (default) - Terminal view
- `--format json` - CI/CD integration
- `--format markdown` - Documentation

## Repository Types

- `root` - Root repository
- `registered` - Repo declared in root `.gitmodules` (legacy alias: `submodule`)
- `unregistered` - Repo not declared in root `.gitmodules` (legacy alias: `standalone`)
- `subrepo` - Umbrella term for non-root repos
- `parent` / `child` / `leaf` - Topology terms used for traversal relationships

Filter with: `--include-types root,registered,unregistered`

## Manifest File

```json
{
  "version": "1.0",
  "workspace_root": ".",
  "repos": [
    {"path": ".", "type": "root"},
    {"path": "vendor/lib", "type": "registered"},
    {"path": "tools/helper", "type": "unregistered"}
  ]
}
```

Create: `./scripts/discover-repos.sh --save manifest.json`

Use: `--manifest manifest.json`

## Error Recovery

### Stash Recovery
```bash
git stash list
git stash show stash@{0}
git stash apply stash@{0}
git stash drop stash@{0}
```

### Rebase Conflicts
```bash
# Resolve conflicts
git add <file>
git rebase --continue

# Or abort
git rebase --abort
```

### Detached HEAD
```bash
git checkout -b recovery-branch
# or
git checkout main
```

## Examples by Use Case

### Single Repository
```bash
# Update
./scripts/update-repo.sh

# Update with different remote
./scripts/update-repo.sh --remote upstream

# Preview
./scripts/update-repo.sh --dry-run
```

### Multiple Repositories
```bash
# Discover
./scripts/discover-repos.sh

# Update all
./scripts/update-workspace-repos.sh

# Update only registered subrepos
./scripts/update-workspace-repos.sh --include-types registered

# Continue on errors
./scripts/update-workspace-repos.sh --continue-on-error
```

### Status & Monitoring
```bash
# Quick status
./scripts/status-all-repos.sh

# Detailed status
./scripts/status-all-repos.sh --check-remote

# JSON output
./scripts/status-all-repos.sh --format json

# Save to file
./scripts/status-all-repos.sh --format markdown --output STATUS.md
```

### Custom Commands
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

### Fork Workflow
```bash
# Clone
./scripts/clone-with-upstream.sh \
  git@github.com:you/fork.git \
  git@github.com:upstream/repo.git

# Sync
./scripts/rebase-to-upstream-latest.sh

# Check
./scripts/foreach-repo.sh "git log upstream/main..HEAD --oneline"
```

## Tips

1. **Always dry-run first**: `--dry-run`
2. **Save manifests**: `--save manifest.json`
3. **Use continue-on-error**: `--continue-on-error`
4. **Check status regularly**: `./scripts/status-all-repos.sh`
5. **Combine scripts**: Chain with `&&`

## Platform Notes

- **Linux/macOS**: Works out of the box
- **Windows**: Use Git Bash
- **All platforms**: Bash 4.0+, Git 2.x+

## Vendor Support

Works with any Git provider:
- GitHub, GitLab, Azure Repos, Bitbucket
- Gitea, Gogs, self-hosted Git
- Any Git-compatible remote

## Getting Help

```bash
# Script help
./scripts/<script-name>.sh --help

# Full documentation
cat docs/README.md
cat docs/USAGE-EXAMPLES.md
```

## Summary

| Need | Script | Command |
|------|--------|---------|
| Update one repo | update-repo.sh | `./scripts/update-repo.sh` |
| Clone fork | clone-with-upstream.sh | `./scripts/clone-with-upstream.sh <fork> <upstream>` |
| Sync with upstream | rebase-to-upstream-latest.sh | `./scripts/rebase-to-upstream-latest.sh` |
| Find repos | discover-repos.sh | `./scripts/discover-repos.sh` |
| Update many repos | update-workspace-repos.sh | `./scripts/update-workspace-repos.sh` |
| Check status | status-all-repos.sh | `./scripts/status-all-repos.sh` |
| Run commands | foreach-repo.sh | `./scripts/foreach-repo.sh "command"` |
