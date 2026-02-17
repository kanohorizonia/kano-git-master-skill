---
name: kano-git-master-skill
description: Comprehensive Git automation toolkit for multi-repository workspaces. Manage root repos, submodules, and standalone repos with vendor-agnostic scripts. Quick updates, fork workflows, batch operations, and status reporting.
version: 0.1.0-beta
---

# Kano Git Master Skill

**Version**: 0.1.0-beta  
**Status**: Beta Release

Comprehensive Git automation scripts for managing multi-repository workspaces. Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.).

## Quick Start

### Update Repository + Submodules (Most Common)

```bash
cd /path/to/your-repo
./scripts/update-repo.sh
```

Updates your repository and all submodules to the latest version with smart branch detection and auto-stash.

### Smart Clone (Clone Fork with Upstream)

```bash
./scripts/smart-clone.sh \
  https://github.com/yourname/fork.git \
  https://github.com/original/repo.git
```

Perfect for contributing to open-source projects.

### Manage Multiple Repositories

```bash
# Discover all repos
./scripts/discover-repos.sh

# Update all repos
./scripts/update-workspace-repos.sh

# Check status
./scripts/status-all-repos.sh
```

## Script Organization

Scripts are organized by category in the `scripts/` directory:

```
scripts/
├── lib/                      # Shared helper library
│   └── git-helpers.sh
├── core/                     # Core operations
│   ├── update-repo.sh
│   ├── smart-clone.sh
│   └── discover-repos.sh
├── workspace/                # Multi-repository operations
│   ├── update-workspace-repos.sh
│   ├── foreach-repo.sh
│   └── status-all-repos.sh
├── branch-operations/        # Branch and commit operations
│   ├── rebase-to-upstream-latest.sh
│   ├── compare-branches.sh
│   └── cherry-pick-batch.sh
└── commit-tools/             # Commit automation
    ├── commit/               # AI-powered commits
    │   ├── smart-commit.sh
    │   └── smart-commit-*.sh (provider wrappers)
    ├── commit-push/          # Complete commit+push workflow
    │   └── smart-commit-push-*.sh
    ├── ignore/               # .gitignore management
    ├── resolve/              # Conflict resolution
    ├── sync/                 # Repository synchronization
    └── smart-push.sh         # Multi-repo push
```

## Core Scripts

| Script | Purpose | Use When |
|--------|---------|----------|
| **update-repo.sh** | Update single repo + submodules | Daily sync, quick updates |
| **smart-clone.sh** | Clone with upstream remote | Fork setup, contribution workflow |
| **init-empty-repo.sh** | Initialize empty remote repo | Quick repo setup, testing |
| **rebase-to-upstream-latest.sh** | Rebase to upstream | Sync with upstream regularly |
| **discover-repos.sh** | Find all repos in workspace | Multi-repo discovery |
| **update-workspace-repos.sh** | Update multiple repos | Batch updates |
| **foreach-repo.sh** | Run commands in all repos | Custom batch operations |
| **status-all-repos.sh** | Generate status report | Monitoring, CI/CD |
| **compare-branches.sh** | Compare commits between branches | Before merge, PR review |
| **cherry-pick-batch.sh** | Batch cherry-pick from file | Selective commit porting |
| **smart-commit-push** | AI commit+push workflow | Multi-repo commits with AI review and auto-push |

## Common Workflows

### Daily Workspace Sync

```bash
./scripts/workspace/status-all-repos.sh && \
./scripts/workspace/update-workspace-repos.sh && \
./scripts/workspace/foreach-repo.sh "git status --short"
```

### Fork Contribution Workflow

```bash
# Initial setup
./scripts/smart-clone.sh <your-fork> <upstream>

# Regular sync
cd project
../scripts/branch-operations/rebase-to-upstream-latest.sh

# Compare branches before PR
../scripts/branch-operations/compare-branches.sh upstream/main HEAD

# Check commits
../scripts/workspace/foreach-repo.sh "git log upstream/main..HEAD --oneline"
```

### Multi-Repository Management

```bash
# Create manifest
./scripts/repo-management/discover-repos.sh --save workspace-manifest.json

# Update all
./scripts/workspace/update-workspace-repos.sh --manifest workspace-manifest.json

# Check status
./scripts/workspace/status-all-repos.sh --manifest workspace-manifest.json --check-remote
```

## Key Features

### Vendor-Agnostic Design
- ✅ Works with any Git remote provider
- ✅ GitHub, GitLab, Azure Repos, Bitbucket
- ✅ Gitea, Gogs, self-hosted Git servers
- ✅ No platform-specific APIs

### Smart Operations
- ✅ Auto-stash/pop local changes
- ✅ Smart branch detection (current or default)
- ✅ Recursive submodule handling
- ✅ Continue-on-error mode
- ✅ Dry-run preview

### Flexible Discovery
- ✅ Root repositories
- ✅ Git submodules
- ✅ Standalone repos in workspace
- ✅ Configurable exclude patterns
- ✅ Manifest file support

### Multiple Output Formats
- ✅ Table (terminal viewing)
- ✅ JSON (CI/CD integration)
- ✅ Markdown (documentation)
- ✅ List (simple output)

## Documentation

- **[Complete Documentation](docs/README.md)** - Full reference with all options
- **[Usage Examples](docs/USAGE-EXAMPLES.md)** - Real-world scenarios and workflows
- **[Quick Reference](docs/QUICK-REFERENCE.md)** - One-page cheat sheet
- **[Testing Guide](TESTING.md)** - Comprehensive testing documentation

## Script Reference

### update-repo.sh (Priority Script)

```bash
# Update current directory
./scripts/update-repo.sh

# Update specific repo
./scripts/update-repo.sh /path/to/repo

# Use different remote
./scripts/update-repo.sh --remote upstream

# Preview changes
./scripts/update-repo.sh --dry-run
```

**Features:** Auto-stash, smart branch detection, recursive submodules, clear progress

### smart-clone.sh

```bash
# Clone without upstream
./scripts/smart-clone.sh https://github.com/user/repo.git

# Clone with upstream
./scripts/smart-clone.sh \
  https://github.com/user/fork.git \
  https://github.com/original/repo.git

# Custom directory
./scripts/smart-clone.sh <url> --dir my-project
```

**Features:** Auto-detect default branch, setup upstream, pull latest, **auto-initialize empty remotes**.

### init-empty-repo.sh

```bash
# Minimal usage - just URL
./scripts/init-empty-repo.sh git@github.com:user/repo.git

# Custom branch
./scripts/init-empty-repo.sh git@github.com:user/repo.git --branch develop

# Custom commit message
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --message "feat: Initial setup"

# Custom file and content
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --file index.html \
  --content "<h1>Hello World</h1>"

# Keep local copy for further work
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --dir ~/my-repo \
  --keep-local

# Force overwrite (DANGEROUS - destroys existing content!)
./scripts/init-empty-repo.sh git@github.com:user/repo.git \
  --force-overwrite-remote
```

**Features:** AI-friendly (all params optional), sensible defaults, quick initialization, custom content

**Safety Features:**
- Pre-checks if remote already has content
- Refuses to push if remote is not empty (unless forced)
- Verbose flag name (`--force-overwrite-remote`) to prevent accidents
- 3-second warning delay before destructive operations
- Rejects old `--force` flag with helpful error message

### rebase-to-upstream-latest.sh

```bash
# Rebase to upstream/main
./scripts/rebase-to-upstream-latest.sh

# Rebase to upstream/develop
./scripts/rebase-to-upstream-latest.sh --branch develop

# Use origin instead
./scripts/rebase-to-upstream-latest.sh --remote origin
```

**Features:** Root + submodules, auto-stash, configurable remote/branch

### discover-repos.sh

```bash
# Discover all repos
./scripts/discover-repos.sh

# Save to manifest
./scripts/discover-repos.sh --save repos-manifest.json

# JSON output
./scripts/discover-repos.sh --format json

# Filter by type
./scripts/discover-repos.sh --include-types standalone
```

**Features:** Find root/submodules/standalone, exclude patterns, multiple formats

### update-workspace-repos.sh

```bash
# Update all repos
./scripts/update-workspace-repos.sh

# Use manifest
./scripts/update-workspace-repos.sh --manifest repos-manifest.json

# Continue on errors
./scripts/update-workspace-repos.sh --continue-on-error

# Filter by type
./scripts/update-workspace-repos.sh --include-types submodule
```

**Features:** Batch updates, manifest support, type filtering, error handling

### foreach-repo.sh

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

**Features:** Execute any command, clear output, continue-on-error

### status-all-repos.sh

```bash
# Table report
./scripts/workspace/status-all-repos.sh

# JSON report
./scripts/workspace/status-all-repos.sh --format json

# Check remote status
./scripts/workspace/status-all-repos.sh --check-remote

# Save to file
./scripts/workspace/status-all-repos.sh --format markdown --output STATUS.md
```

**Features:** Multiple formats, remote checking, file output, summary stats

### compare-branches.sh

```bash
# Basic comparison
./scripts/branch-operations/compare-branches.sh main feature/new

# Bidirectional comparison
./scripts/branch-operations/compare-branches.sh main develop --bidirectional

# Detailed output with file changes
./scripts/branch-operations/compare-branches.sh main feature/new --detailed

# JSON output
./scripts/branch-operations/compare-branches.sh main feature/new --format json

# Save to markdown file
./scripts/branch-operations/compare-branches.sh main feature/new --format markdown --output diff.md
```

**Features:** Bidirectional comparison, multiple formats (table/JSON/markdown), detailed file changes, commit metadata

### cherry-pick-batch.sh

```bash
# Cherry-pick from JSON file
./scripts/branch-operations/cherry-pick-batch.sh commits.json

# Cherry-pick from text file
./scripts/branch-operations/cherry-pick-batch.sh commits.txt

# Preview without applying
./scripts/branch-operations/cherry-pick-batch.sh commits.json --dry-run

# Continue after resolving conflicts
./scripts/branch-operations/cherry-pick-batch.sh commits.json --continue

# Abort operation
./scripts/branch-operations/cherry-pick-batch.sh commits.json --abort
```

**File formats:**
- JSON: Structured format with hash, title, author, date
- Text: Simple format with hash and optional title
- One hash per line

**Features:** Batch cherry-pick, structured file support, conflict handling, validation

### smart-commit.sh

```bash
# Basic usage (free model)
./scripts/commit-tools/smart-commit.sh

# Use specific model
./scripts/commit-tools/smart-commit.sh --model gpt-5-mini

# Custom message
./scripts/commit-tools/smart-commit.sh -m "feat: Add new feature"

# Commit and push
./scripts/commit-tools/smart-commit.sh --push

# Skip AI review (only static checks)
./scripts/commit-tools/smart-commit.sh --no-ai-review
```

**Features:** AI-generated commit messages, safety checks (secrets, large files), auto .gitignore updates, AI review gate, multi-repo support

## Common Options

All scripts support:
- `--help` - Show help message
- `--dry-run` - Preview mode (no changes)

Most scripts support:
- `--remote <name>` - Remote name (default: origin or upstream)
- `--manifest <file>` - Use manifest file
- `--include-types <types>` - Filter by type (root,submodule,standalone)
- `--exclude <pattern>` - Exclude patterns (repeatable)
- `--continue-on-error` - Don't stop on failures

## Best Practices

1. **Use dry-run first**: Always preview with `--dry-run`
2. **Save manifests**: Create reusable manifests with `--save`
3. **Check status regularly**: Monitor with `status-all-repos.sh`
4. **Continue on errors**: Use `--continue-on-error` for batch operations
5. **Combine scripts**: Chain scripts with `&&` for workflows
6. **Use verbose mode for debugging**: Add `--verbose` to see all repos (default shows only changes)
7. **Review summary tables**: Check commit and push summaries after workflow completion

## Troubleshooting

### Rebase Conflicts
```bash
git status                    # Check conflicts
vim conflicted-file.txt       # Resolve
git add conflicted-file.txt   # Mark resolved
git rebase --continue         # Continue
git stash pop stash@{0}       # Restore stash
```

### Stash Recovery
```bash
git stash list                # List stashes
git stash show stash@{0}      # Show content
git stash apply stash@{0}     # Apply
git stash drop stash@{0}      # Drop after success
```

### Detached HEAD
```bash
git checkout -b recovery-branch  # Create branch
# or
git checkout main                # Checkout known branch
```

## Platform Support

- **Linux/macOS**: Works out of the box (Bash 4.0+)
- **Windows**: Use Git Bash for Windows
- **All platforms**: Requires Git 2.x+

## Shared Helper Library

All scripts use `git-helpers.sh` for consistent behavior:

- **Stash management**: Create, pop, check changes
- **Branch operations**: Get current, get default, check existence
- **Repository discovery**: Find repos, collect metadata
- **Remote operations**: Check existence, fetch
- **Utilities**: Logging, dry-run, exclude patterns

Function prefix: `gith_` (git-helper, not GitHub-specific)

## Getting Help

```bash
# Script help
./scripts/repo-management/update-repo.sh --help
./scripts/repo-management/discover-repos.sh --help
./scripts/workspace/update-workspace-repos.sh --help
./scripts/branch-operations/compare-branches.sh --help
./scripts/branch-operations/cherry-pick-batch.sh --help
./scripts/commit-tools/smart-commit.sh --help

# Documentation
cat docs/README.md
cat docs/USAGE-EXAMPLES.md
cat docs/QUICK-REFERENCE.md
```

## New Features

### Branch Comparison (compare-branches.sh)

Compare commits between two branches to understand differences before merging or cherry-picking:

```bash
# See what's in feature branch but not in main
./scripts/branch-operations/compare-branches.sh main feature/new

# Bidirectional comparison
./scripts/branch-operations/compare-branches.sh main develop --bidirectional

# Export to markdown for documentation
./scripts/branch-operations/compare-branches.sh main feature/new \
  --format markdown \
  --output branch-diff.md
```

### Batch Cherry-Pick (cherry-pick-batch.sh)

Cherry-pick multiple commits from a structured file:

**Create commits file (JSON):**
```json
{
  "commits": [
    {
      "hash": "abc123",
      "title": "feat: Add new feature",
      "author": "John Doe",
      "date": "2024-01-15"
    },
    {
      "hash": "def456",
      "title": "fix: Bug fix"
    }
  ]
}
```

**Or simple text format:**
```
abc123 feat: Add new feature
def456 fix: Bug fix
```

**Execute:**
```bash
# Preview first
./scripts/branch-operations/cherry-pick-batch.sh commits.json --dry-run

# Apply
./scripts/branch-operations/cherry-pick-batch.sh commits.json
```

### Smart Commit (smart-commit.sh)

AI-powered commit across all repositories with safety checks:

```bash
# Use Copilot provider (recommended)
./scripts/commit-tools/commit/smart-commit-copilot.sh

# Or use with specific provider
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini

# Custom message for all repos
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini -m "chore: Update dependencies"

# Show detailed output (default: only shows repos with changes)
./scripts/commit-tools/commit/smart-commit.sh --provider copilot --model gpt-5-mini --verbose
```

**Agent delegation note (required contract):**
- For delegated execution, pass `--agent <name>` (for example: `codex`, `cursor`, `copilot`, `kiro`, `claude`).
- When `--agent` is not `manual`, a fixed commit message (`-m/--message`) is required.
- Delegated runs disable in-script AI review (`--no-ai-review`) to avoid duplicate model calls.

**Output modes:**
- **Default (quiet)**: Shows only repos with actual commits
- **Verbose**: Shows all repos, including those with no changes

**Safety features:**
- Detects secrets, API keys, private keys
- Blocks large files
- Auto-updates .gitignore (only prints when file actually changes)
- AI safety review (optional, skipped when no changes)
- Works with GitHub Copilot CLI

### Smart Commit-Push (smart-commit-push.sh)

Complete workflow: commit and push all repositories in one step:

```bash
# Full workflow with Copilot
./scripts/commit-tools/commit-push/smart-commit-push-copilot.sh

# Or specify provider manually
./scripts/commit-tools/commit-push/smart-commit-push.sh --provider copilot --model gpt-5-mini

# With verbose output
./scripts/commit-tools/commit-push/smart-commit-push.sh --provider copilot --model gpt-5-mini --verbose
```

**Agent delegation note (required contract):**
- For agent-driven workflows, pass `--agent <name>` and provide a fixed commit message (`-m "..."`).
- When `--agent` is not `manual`, delegated mode disables in-script AI review (`--no-ai-review`) to avoid duplicate model usage.

**Summary tables:**
After successful completion, displays two summary tables:

1. **Commit Summary**: Shows which repos were committed, how many commits, and branch name
2. **Push Summary**: Shows which repos were pushed, to which remote, and branch name

Example output:
```
=== Commit Summary ===
Repository                    Commits  Branch
-----------                   -------  ------
kano-git-master-skill         1        main
skills/kano                   1        dev/tooling

=== Push Summary ===
Repository                    Remote             Branch
-----------                   ------             ------
kano-git-master-skill         origin             main
skills/kano                   origin-http        dev/tooling
backlog                       origin             main (no changes)
```

**Features:**
- Processes root repo, submodules, and nested repos
- AI-generated commit messages with safety review
- Automatic sync with upstream before push
- SSH→HTTP fallback per repository
- Summary statistics on completion

## Legacy Scripts

### ai-safe-commit-all-repos.sh (Deprecated)

This script has been replaced by `smart-commit.sh` with improved features:
- Better Copilot CLI detection
- Free tier model defaults
- Clearer error messages
- Simplified naming

Use `smart-commit.sh` instead.

## Summary

The Git Master Skill provides a complete toolkit for multi-repository management:

- **Quick updates**: `update-repo.sh` for fast single-repo updates
- **Fork workflows**: `clone-with-upstream.sh` and `rebase-to-upstream-latest.sh`
- **Multi-repo management**: Discovery, batch updates, status reporting
- **Batch operations**: Execute custom commands across all repos
- **Vendor-agnostic**: Works with any Git provider
- **Production-ready**: Error handling, dry-run, logging, stash management

Start with `update-repo.sh` for simple cases, then explore the full suite for complex workflows.

## Quick Reference

| Need | Command |
|------|---------|
| Update one repo | `./scripts/repo-management/update-repo.sh` |
| Clone fork | `./scripts/smart-clone.sh <fork> <upstream>` |
| Sync with upstream | `./scripts/branch-operations/rebase-to-upstream-latest.sh` |
| Compare branches | `./scripts/branch-operations/compare-branches.sh <base> <compare>` |
| Batch cherry-pick | `./scripts/branch-operations/cherry-pick-batch.sh <file>` |
| Find all repos | `./scripts/repo-management/discover-repos.sh` |
| Update all repos | `./scripts/workspace/update-workspace-repos.sh` |
| Check status | `./scripts/workspace/status-all-repos.sh` |
| Run command | `./scripts/workspace/foreach-repo.sh "command"` |
| Smart commit | `./scripts/commit-tools/smart-commit.sh` |

For detailed examples and workflows, see [docs/README.md](docs/README.md).

## Testing

Comprehensive test suite available:

```bash
# Quick smoke test (30 seconds)
bash scripts/test/quick-test.sh

# Full test suite (5-10 minutes)
bash scripts/test/run-all-tests.sh \
  --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --cleanup
```

See [TESTING.md](TESTING.md) for complete testing documentation.
