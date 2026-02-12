# Git Master Skill - Complete Documentation

A comprehensive suite of Git automation scripts for managing multi-repository workspaces. Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.).

## Table of Contents

- [Quick Start](#quick-start)
- [Core Scripts](#core-scripts)
- [Installation](#installation)
- [Common Workflows](#common-workflows)
- [Script Reference](#script-reference)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

## Quick Start

### Update a Repository and Its Submodules

```bash
cd /path/to/your-repo
./scripts/update-repo.sh
```

This is the most common operation - updates your repository and all submodules to the latest version.

### Clone a Fork with Upstream

```bash
./scripts/clone-with-upstream.sh \
  https://github.com/yourname/fork.git \
  https://github.com/original/repo.git
```

Perfect for contributing to open-source projects.

### Manage Multiple Repositories

```bash
# Discover all repos in workspace
./scripts/discover-repos.sh

# Update all repos
./scripts/update-workspace-repos.sh

# Check status of all repos
./scripts/status-all-repos.sh
```

## Core Scripts

### 1. update-repo.sh (Priority Script)

**Purpose:** Quickly update a single repository and all its submodules.

**Usage:**
```bash
./scripts/update-repo.sh [path] [options]
```

**Options:**
- `--remote <name>` - Remote name (default: origin)
- `--no-stash` - Fail if there are local changes
- `--dry-run` - Preview mode
- `-h, --help` - Show help

**Examples:**
```bash
# Update current directory
./scripts/update-repo.sh

# Update specific repository
./scripts/update-repo.sh /path/to/repo

# Use different remote
./scripts/update-repo.sh --remote upstream

# Preview changes
./scripts/update-repo.sh --dry-run
```

**Features:**
- ✅ Auto-stash/pop local changes
- ✅ Smart branch detection
- ✅ Recursive submodule updates
- ✅ Clear progress output
- ✅ Works with any Git provider

### 2. clone-with-upstream.sh

**Purpose:** Clone a repository and optionally set up upstream remote.

**Usage:**
```bash
./scripts/clone-with-upstream.sh <repo-url> [upstream-url] [options]
```

**Options:**
- `--dir <path>` - Target directory
- `--no-checkout` - Skip checkout to default branch
- `--dry-run` - Preview mode
- `-h, --help` - Show help

**Examples:**
```bash
# Clone without upstream
./scripts/clone-with-upstream.sh https://github.com/user/repo.git

# Clone with upstream (for forks)
./scripts/clone-with-upstream.sh \
  https://github.com/user/fork.git \
  https://github.com/original/repo.git

# Clone to custom directory
./scripts/clone-with-upstream.sh \
  https://github.com/user/repo.git \
  --dir my-project
```

**Features:**
- ✅ Auto-detects default branch
- ✅ Sets up upstream remote
- ✅ Pulls latest changes
- ✅ Custom directory support

### 3. rebase-to-upstream-latest.sh

**Purpose:** Rebase current branch to upstream's latest.

**Usage:**
```bash
./scripts/rebase-to-upstream-latest.sh [options]
```

**Options:**
- `--branch <name>` - Base branch (default: main)
- `--remote <name>` - Remote name (default: upstream)
- `--detached <checkout|skip>` - Detached HEAD behavior
- `--no-stash` - Fail if there are local changes
- `-h, --help` - Show help

**Examples:**
```bash
# Rebase to upstream/main
./scripts/rebase-to-upstream-latest.sh

# Rebase to upstream/develop
./scripts/rebase-to-upstream-latest.sh --branch develop

# Rebase to origin instead
./scripts/rebase-to-upstream-latest.sh --remote origin
```

**Features:**
- ✅ Works with root + all submodules
- ✅ Auto-stash/pop local changes
- ✅ Configurable remote and branch
- ✅ Handles detached HEAD

### 4. discover-repos.sh

**Purpose:** Discover all Git repositories in workspace.

**Usage:**
```bash
./scripts/discover-repos.sh [options]
```

**Options:**
- `--root <path>` - Search root (default: current dir)
- `--max-depth <n>` - Max search depth (default: 3)
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--format <json|list>` - Output format (default: list)
- `--save <file>` - Save to manifest file
- `--include-types <types>` - Filter by type (root,submodule,standalone)
- `--dry-run` - Preview mode
- `-h, --help` - Show help

**Examples:**
```bash
# Discover all repos
./scripts/discover-repos.sh

# Save to manifest
./scripts/discover-repos.sh --save repos-manifest.json

# JSON output
./scripts/discover-repos.sh --format json

# Only standalone repos
./scripts/discover-repos.sh --include-types standalone

# Custom depth and excludes
./scripts/discover-repos.sh \
  --max-depth 5 \
  --exclude node_modules \
  --exclude .cache
```

**Features:**
- ✅ Discovers root, submodules, standalone repos
- ✅ Configurable exclude patterns
- ✅ Multiple output formats
- ✅ Manifest file support

### 5. update-workspace-repos.sh

**Purpose:** Update all repositories in workspace.

**Usage:**
```bash
./scripts/update-workspace-repos.sh [options]
```

**Options:**
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--remote <name>` - Remote name (default: origin)
- `--max-depth <n>` - Discovery depth (default: 3)
- `--parallel <n>` - Parallel updates (default: 1)
- `--continue-on-error` - Continue on failures
- `--dry-run` - Preview mode
- `-h, --help` - Show help

**Examples:**
```bash
# Update all repos
./scripts/update-workspace-repos.sh

# Use manifest file
./scripts/update-workspace-repos.sh --manifest repos-manifest.json

# Update only standalone repos
./scripts/update-workspace-repos.sh --include-types standalone

# Continue on errors
./scripts/update-workspace-repos.sh --continue-on-error

# Use different remote
./scripts/update-workspace-repos.sh --remote upstream
```

**Features:**
- ✅ Batch update multiple repos
- ✅ Manifest or auto-discovery
- ✅ Type filtering
- ✅ Continue-on-error mode
- ✅ Summary reporting

### 6. foreach-repo.sh

**Purpose:** Execute commands across all repositories.

**Usage:**
```bash
./scripts/foreach-repo.sh <command> [options]
```

**Options:**
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--max-depth <n>` - Discovery depth (default: 3)
- `--continue-on-error` - Continue on failures
- `--parallel <n>` - Parallel execution (default: 1)
- `--dry-run` - Preview mode
- `-h, --help` - Show help

**Examples:**
```bash
# Check status of all repos
./scripts/foreach-repo.sh "git status --short"

# Check for unpushed commits
./scripts/foreach-repo.sh "git log origin/main..HEAD --oneline"

# Create branch in all repos
./scripts/foreach-repo.sh "git checkout -b feature/new-feature"

# Fetch all remotes
./scripts/foreach-repo.sh "git fetch --all --prune"

# Run tests in all repos
./scripts/foreach-repo.sh "npm test" --continue-on-error
```

**Features:**
- ✅ Execute any command in all repos
- ✅ Clear output with repo context
- ✅ Continue-on-error support
- ✅ Type filtering

### 7. status-all-repos.sh

**Purpose:** Generate status report for all repositories.

**Usage:**
```bash
./scripts/status-all-repos.sh [options]
```

**Options:**
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--max-depth <n>` - Discovery depth (default: 3)
- `--format <table|json|markdown>` - Output format (default: table)
- `--check-remote` - Check remote status (slower)
- `--output <file>` - Save to file
- `-h, --help` - Show help

**Examples:**
```bash
# Table report
./scripts/status-all-repos.sh

# JSON report
./scripts/status-all-repos.sh --format json

# Check remote status
./scripts/status-all-repos.sh --check-remote

# Save to file
./scripts/status-all-repos.sh \
  --format markdown \
  --output STATUS.md

# Detailed JSON for CI/CD
./scripts/status-all-repos.sh \
  --check-remote \
  --format json \
  --output status.json
```

**Features:**
- ✅ Shows branch, changes, unpushed commits
- ✅ Multiple output formats
- ✅ Optional remote checking
- ✅ File output support

## Installation

### Prerequisites

- Git 2.x or higher
- Bash 4.x or higher
- Unix shell or Git Bash on Windows

### Setup

1. Clone or download the kano-git-master-skill repository
2. Scripts are located in `skills/kano-git-master-skill/scripts/`
3. Make scripts executable (if needed):

```bash
chmod +x skills/kano-git-master-skill/scripts/*.sh
```

### Verify Installation

```bash
# Test help output
./scripts/update-repo.sh --help
./scripts/discover-repos.sh --help
```

## Common Workflows

### Workflow 1: Daily Workspace Sync

```bash
# 1. Check current status
./scripts/status-all-repos.sh

# 2. Update all repos
./scripts/update-workspace-repos.sh

# 3. Verify updates
./scripts/foreach-repo.sh "git status --short"
```

### Workflow 2: Fork Contribution

```bash
# Initial setup
./scripts/clone-with-upstream.sh \
  git@github.com:yourname/project.git \
  git@github.com:upstream/project.git

cd project

# Regular sync
../scripts/rebase-to-upstream-latest.sh

# Before creating PR
../scripts/foreach-repo.sh "git log upstream/main..HEAD --oneline"
```

### Workflow 3: Multi-Repo Management

```bash
# Create manifest
./scripts/discover-repos.sh --save workspace-manifest.json

# Check status
./scripts/status-all-repos.sh \
  --manifest workspace-manifest.json \
  --check-remote

# Update all
./scripts/update-workspace-repos.sh \
  --manifest workspace-manifest.json

# Run tests
./scripts/foreach-repo.sh "npm test" \
  --manifest workspace-manifest.json \
  --continue-on-error
```

### Workflow 4: Selective Updates

```bash
# Update only submodules
./scripts/update-workspace-repos.sh --include-types submodule

# Update only standalone repos
./scripts/update-workspace-repos.sh --include-types standalone

# Update with excludes
./scripts/update-workspace-repos.sh \
  --exclude vendor \
  --exclude node_modules
```

## Script Reference

### Shared Helper Library (git-helpers.sh)

All scripts use a shared helper library for consistent behavior:

**Stash Management:**
- `gith_has_changes()` - Check for uncommitted changes
- `gith_stash_create()` - Create stash with tracking
- `gith_stash_pop()` - Pop stash with error handling

**Branch Operations:**
- `gith_get_current_branch()` - Get current branch
- `gith_get_default_branch()` - Detect default branch
- `gith_branch_exists_on_remote()` - Check remote branch

**Repository Discovery:**
- `gith_is_git_repo()` - Check if directory is git repo
- `gith_discover_repos()` - Discover all repos
- `gith_collect_submodules()` - Collect submodules
- `gith_collect_repo_metadata()` - Gather repo info

**Remote Operations:**
- `gith_has_remote()` - Check if remote exists
- `gith_fetch_remote()` - Fetch with error handling

**Utilities:**
- `gith_run()` - Dry-run wrapper
- `gith_log()` - Consistent logging
- `gith_error()` - Error logging
- `gith_is_excluded()` - Check exclude patterns

### Manifest File Format

```json
{
  "version": "1.0",
  "workspace_root": ".",
  "generated_at": "2026-02-12T10:30:00Z",
  "repos": [
    {
      "path": ".",
      "type": "root",
      "current_branch": "main",
      "remotes": "origin,upstream",
      "has_changes": false
    },
    {
      "path": "vendor/lib",
      "type": "submodule",
      "current_branch": "main",
      "remotes": "origin",
      "has_changes": false
    },
    {
      "path": "tools/helper",
      "type": "standalone",
      "current_branch": "develop",
      "remotes": "origin",
      "has_changes": true
    }
  ]
}
```

## Best Practices

### 1. Use Dry-Run First

Always preview changes before executing:

```bash
./scripts/update-workspace-repos.sh --dry-run
```

### 2. Save Manifests

Create manifests for repeatable operations:

```bash
./scripts/discover-repos.sh --save .workspace-manifest.json
echo ".workspace-manifest.json" >> .gitignore
```

### 3. Regular Status Checks

Check status regularly to catch issues early:

```bash
# Add to your shell profile
alias repo-status='./scripts/status-all-repos.sh --check-remote'
```

### 4. Use Continue-on-Error for Batch Operations

Don't stop on first failure when updating many repos:

```bash
./scripts/update-workspace-repos.sh --continue-on-error
```

### 5. Combine Scripts

Chain scripts for powerful workflows:

```bash
./scripts/discover-repos.sh --save manifest.json && \
./scripts/status-all-repos.sh --manifest manifest.json && \
./scripts/update-workspace-repos.sh --manifest manifest.json
```

### 6. Automate with Cron

Schedule regular updates:

```bash
# Add to crontab
0 9 * * * cd /path/to/workspace && ./scripts/update-workspace-repos.sh
```

### 7. Integrate with CI/CD

Use in GitHub Actions, GitLab CI, etc.:

```yaml
- name: Update repositories
  run: ./scripts/update-workspace-repos.sh --continue-on-error
```

## Troubleshooting

### Rebase Conflicts

If rebase fails with conflicts:

```bash
# Check status
git status

# Resolve conflicts
vim conflicted-file.txt
git add conflicted-file.txt

# Continue rebase
git rebase --continue

# Restore stash if needed
git stash pop stash@{0}
```

### Stash Recovery

If script fails and leaves stash:

```bash
# List stashes
git stash list

# Show stash content
git stash show stash@{0} -p

# Apply stash
git stash apply stash@{0}

# Drop after successful apply
git stash drop stash@{0}
```

### Detached HEAD

If repository is in detached HEAD:

```bash
# Create branch from current position
git checkout -b recovery-branch

# Or checkout to known branch
git checkout main
```

### Remote Not Found

If remote doesn't exist:

```bash
# Check remotes
git remote -v

# Add missing remote
git remote add upstream https://github.com/original/repo.git

# Fetch
git fetch upstream
```

### Submodule Issues

If submodules are not initialized:

```bash
# Initialize all submodules
git submodule update --init --recursive
```

## Platform Support

### Linux / macOS

Works out of the box with Bash 4.0+.

### Windows (Git Bash)

All scripts work on Git Bash for Windows:

```bash
# Use forward slashes
./scripts/update-repo.sh /c/Users/username/projects/repo

# Or Windows-style paths (auto-converted)
./scripts/update-repo.sh "C:\Users\username\projects\repo"
```

## Vendor-Agnostic Design

All scripts work with any Git remote provider:

- ✅ GitHub
- ✅ GitLab
- ✅ Azure Repos
- ✅ Bitbucket
- ✅ Gitea / Gogs
- ✅ Self-hosted Git servers
- ✅ Any Git-compatible remote

No platform-specific APIs or assumptions are made.

## Getting Help

### Script Help

All scripts have built-in help:

```bash
./scripts/update-repo.sh --help
./scripts/discover-repos.sh --help
./scripts/update-workspace-repos.sh --help
./scripts/foreach-repo.sh --help
./scripts/status-all-repos.sh --help
./scripts/rebase-to-upstream-latest.sh --help
./scripts/clone-with-upstream.sh --help
```

### Documentation

- [Usage Examples](./USAGE-EXAMPLES.md) - Detailed examples and scenarios
- [SKILL.md](../SKILL.md) - Skill overview and quick reference

## Summary

The Git Master Skill provides a complete toolkit for managing multi-repository workspaces:

- **Quick Updates**: `update-repo.sh` for fast single-repo updates
- **Fork Management**: `clone-with-upstream.sh` and `rebase-to-upstream-latest.sh`
- **Multi-Repo**: `discover-repos.sh`, `update-workspace-repos.sh`, `status-all-repos.sh`
- **Batch Operations**: `foreach-repo.sh` for custom commands
- **Vendor-Agnostic**: Works with any Git provider
- **Production-Ready**: Error handling, dry-run, logging, stash management

Start with `update-repo.sh` for simple cases, then explore the full suite for complex workflows.
