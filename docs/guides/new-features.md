# Git Master Skill - New Features Guide

**Version**: 2.0
**Last Updated**: 2026-02-12

## Overview

This document provides a quick reference to all new features added to kano-git-master-skill. For detailed documentation, see the individual guides linked below.

## Quick Navigation

- [Version Information](#version-information)
- [Worktree Management](#worktree-management)
- [Subtree Management](#subtree-management)
- [Submodule Enhancement](#submodule-enhancement)
- [Mono-repo Tools (Git Scalar)](#mono-repo-tools-git-scalar)
- [VCS Bridges - Git-P4](#vcs-bridges---git-p4)
- [VCS Bridges - Git-SVN](#vcs-bridges---git-svn)
- [Documentation Guides](#documentation-guides)

## Version Information

Extract version information from git, git-p4, or git-svn repositories.

### Scripts
- `scripts/core/get-version-info.sh` - Extract version information

### Features
- Extract Git hash, branch, revision count, tag
- Parse git-p4 metadata (depot, stream, project, change)
- Parse git-svn metadata (URL, revision, branch)
- Revision offset for marketplace publishing
- Multiple output formats (export, env, text, JSON)

### Quick Start
```bash
# Get version info
./scripts/core/get-version-info.sh --export

# With revision offset
./scripts/core/get-version-info.sh --export --offset -500000

# JSON format
./scripts/core/get-version-info.sh --format json
```

### Documentation
- [Version Info Guide](./version-info.md)
- Revision offset implementation notes (historical)

---

## Worktree Management

Manage multiple working trees for parallel development.

### Scripts
- `scripts/worktree/create-worktree.sh` - Create worktree
- `scripts/worktree/create-orphan-worktree.sh` - Create orphan branch + worktree
- `scripts/worktree/list-worktrees.sh` - List all worktrees
- `scripts/worktree/remove-worktree.sh` - Remove worktree
- `scripts/worktree/sync-worktrees.sh` - Sync all worktrees
- `scripts/worktree/open-worktree.sh` - Open in IDE

### Features
- Create worktree for any branch (existing or new)
- Create orphan branches with worktrees in one step
- Auto-generate worktree paths
- List worktrees with metadata (table, JSON)
- Safe removal with uncommitted change checks
- Batch sync operations
- IDE integration (VS Code, IntelliJ, Vim)

### Quick Start
```bash
# Create worktree
./scripts/worktree/create-worktree.sh feature-branch

# Create orphan branch + worktree
./scripts/worktree/create-orphan-worktree.sh docs --open

# List all worktrees
./scripts/worktree/list-worktrees.sh

# Sync all worktrees
./scripts/worktree/sync-worktrees.sh --status
```

### Use Cases
- Documentation management (GitHub Pages)
- Multi-project mono-repo
- Configuration management
- Localization (i18n)
- Experimental features

### Documentation
- Worktree implementation notes (historical)
- [Worktree and Scalar Design](../design/worktree-scalar.md)
- [Orphan Branch Design](../design/orphan-branch.md)

---

## Subtree Management

Include external repositories as subdirectories.

### Scripts
- `scripts/subtree/add-subtree.sh` - Add subtree
- `scripts/subtree/pull-subtree.sh` - Pull updates
- `scripts/subtree/push-subtree.sh` - Push changes
- `scripts/subtree/split-subtree.sh` - Split to new branch
- `scripts/subtree/list-subtrees.sh` - List all subtrees

### Features
- Add subtree from another repository
- Pull updates from subtree source
- Push changes back to subtree source
- Split subtree history to new branch
- List all subtrees with metadata
- Auto-detect remote URLs
- Squash support

### Quick Start
```bash
# Add subtree
./scripts/subtree/add-subtree.sh \
  --prefix lib/mylib \
  --url https://github.com/user/mylib.git

# Pull updates
./scripts/subtree/pull-subtree.sh --prefix lib/mylib

# Push changes
./scripts/subtree/push-subtree.sh \
  --prefix lib/mylib \
  --url https://github.com/user/mylib.git

# List all subtrees
./scripts/subtree/list-subtrees.sh
```

### Use Cases
- Vendor external dependencies
- Include libraries without submodules
- Contribute changes back to upstream
- Extract subtree to separate repository

### Documentation
- Subtree implementation notes (historical)
- [Submodule vs Subtree](../comparisons/submodule-vs-subtree.md) - Comparison guide

---

## Submodule Enhancement

Enhanced submodule management with better UX.

### Scripts
- `scripts/submodules/add-submodule.sh` - Add submodule
- `scripts/submodules/update-submodules.sh` - Update all submodules
- `scripts/submodules/remove-submodule.sh` - Remove submodule
- `scripts/submodules/foreach-submodule.sh` - Execute command in submodules

### Features
- Add submodule with branch tracking
- Update all submodules (recursive, remote)
- Safe removal with cleanup
- Execute commands in all submodules
- Batch operations

### Quick Start
```bash
# Add submodule
./scripts/submodules/add-submodule.sh \
  --url https://github.com/user/lib.git \
  --path lib/mylib

# Update all submodules
./scripts/submodules/update-submodules.sh --remote --recursive

# Remove submodule
./scripts/submodules/remove-submodule.sh lib/mylib

# Execute command in all submodules
./scripts/submodules/foreach-submodule.sh "git status"
```

### Use Cases
- Shared libraries across projects
- Plugin systems
- Microservices dependencies
- Build tools

### Documentation
- Submodule implementation notes (historical)
- [Submodule vs Subtree](../comparisons/submodule-vs-subtree.md) - Comparison guide

---

## Mono-repo Tools (Git Scalar)

Optimize large repositories with Git Scalar.

### Scripts
- `scripts/mono-repo/scalar/register.sh` - Register with Scalar
- `scripts/mono-repo/scalar/status.sh` - Show Scalar status
- `scripts/mono-repo/scalar/optimize.sh` - Run optimizations
- `scripts/mono-repo/scalar/unregister.sh` - Unregister from Scalar

### Features
- 10-20x performance improvements
- Partial clone (blob:none filter)
- Sparse checkout (cone mode)
- Background maintenance (hourly/daily)
- FSMonitor (file system monitoring)
- Multi-pack index
- Commit graph

### Quick Start
```bash
# Register with Scalar
./scripts/mono-repo/scalar/register.sh

# Check status
./scripts/mono-repo/scalar/status.sh

# Run optimizations
./scripts/mono-repo/scalar/optimize.sh

# Unregister
./scripts/mono-repo/scalar/unregister.sh
```

### Performance Impact
- Initial clone: 10-20x faster
- git status: 5-10x faster
- git checkout: 3-5x faster
- Disk usage: 50-90% reduction

### Use Cases
- Repositories > 1GB
- Repositories > 100,000 files
- Mono-repos with multiple projects
- CI/CD pipelines

### Documentation
- Scalar implementation notes (historical)
- [When to Use Scalar](../comparisons/when-to-use-scalar.md) - Decision guide

---

## VCS Bridges - Git-P4

Bidirectional sync between Git and Perforce.

### Scripts
- `scripts/vcs-bridges/p4/clone.sh` - Clone from Perforce
- `scripts/vcs-bridges/p4/sync.sh` - Sync from Perforce
- `scripts/vcs-bridges/p4/submit.sh` - Submit to Perforce
- `scripts/vcs-bridges/p4/rebase.sh` - Rebase with Perforce
- `scripts/vcs-bridges/p4/strip-metadata.sh` - Strip git-p4 metadata

### Features
- Clone Perforce depot to Git
- Sync changes from Perforce
- Submit Git commits to Perforce
- Rebase on Perforce changes
- Strip git-p4 metadata
- Python 3 validation
- Environment validation

### Quick Start
```bash
# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Strip metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~10..HEAD
```

### Use Cases
- Perforce to Git migration
- Hybrid Perforce/Git workflow
- Multi-branch workflows
- Cherry-picking between P4-synced branches

### Documentation
- VCS bridge implementation notes (historical)
- [Git-P4 vs Git-SVN](../comparisons/git-p4-vs-git-svn.md) - Comparison guide
- [Perforce Migration](../migrations/perforce-to-git.md) - Migration guide

---

## VCS Bridges - Git-SVN

Bidirectional sync between Git and Subversion.

### Scripts
- `scripts/vcs-bridges/svn/clone.sh` - Clone from Subversion
- `scripts/vcs-bridges/svn/fetch.sh` - Fetch from Subversion
- `scripts/vcs-bridges/svn/dcommit.sh` - Commit to Subversion
- `scripts/vcs-bridges/svn/rebase.sh` - Rebase with Subversion

### Features
- Clone SVN repository to Git
- Fetch changes from SVN
- Commit Git changes to SVN
- Rebase on SVN changes
- Standard and custom layouts
- Branch and tag mapping

### Quick Start
```bash
# Clone from SVN (standard layout)
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Clone with custom layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
  --trunk main --branches feature --tags releases

# Fetch from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Rebase on SVN
./scripts/vcs-bridges/svn/rebase.sh

# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh
```

### Use Cases
- SVN to Git migration
- Hybrid SVN/Git workflow
- Legacy system integration
- Team collaboration across VCS

### Documentation
- VCS bridge implementation notes (historical)
- [Git-P4 vs Git-SVN](../comparisons/git-p4-vs-git-svn.md) - Comparison guide
- [SVN Migration](../migrations/svn-to-git.md) - Migration guide

---

## Documentation Guides

### Comparison Guides
- [Submodule vs Subtree](../comparisons/submodule-vs-subtree.md) - Detailed comparison with decision matrix
- [When to Use Scalar](../comparisons/when-to-use-scalar.md) - Scalar decision guide with thresholds
- [Git-P4 vs Git-SVN](../comparisons/git-p4-vs-git-svn.md) - VCS bridge comparison

### Migration Guides
- [Perforce Migration](../migrations/perforce-to-git.md) - Complete Perforce to Git migration guide
- [SVN Migration](../migrations/svn-to-git.md) - Complete SVN to Git migration guide

### Phase Completion Guides
- Phase 1 complete notes (historical) - Folder restructure
- Phase 2 complete notes (historical) - Worktree management
- Phase 3 complete notes (historical) - Subtree management
- Phase 4-7 complete notes (historical) - Submodule, Scalar, VCS bridges

### Status Documents
- Implementation status notes (historical) - Ongoing status tracking
- Implementation complete notes (historical) - Implementation summary
- Final summary notes (historical) - Final summary

### Design Documents
- [Worktree and Scalar Design](../design/worktree-scalar.md) - Worktree and Scalar design
- [Orphan Branch Design](../design/orphan-branch.md) - Orphan branch integration
- [Version Info Guide](./version-info.md) - Version extraction guide
- Revision offset feature notes (historical)

---

## Common Workflows

### Workflow 1: Parallel Development with Worktrees

```bash
# Create worktree for feature
./scripts/worktree/create-worktree.sh feature-auth

# Work in feature worktree
cd ../myproject-feature-auth
# ... make changes ...
git commit -m "Add authentication"

# Create worktree for bugfix
./scripts/worktree/create-worktree.sh bugfix-login

# Work in bugfix worktree
cd ../myproject-bugfix-login
# ... fix bug ...
git commit -m "Fix login issue"

# List all worktrees
./scripts/worktree/list-worktrees.sh
```

### Workflow 2: Vendor External Library with Subtree

```bash
# Add external library as subtree
./scripts/subtree/add-subtree.sh \
  --prefix lib/awesome-lib \
  --url https://github.com/awesome/lib.git

# Pull updates from upstream
./scripts/subtree/pull-subtree.sh --prefix lib/awesome-lib

# Make local changes
cd lib/awesome-lib
# ... make changes ...
cd ../..
git commit -m "Customize awesome-lib"

# Push changes back to upstream
./scripts/subtree/push-subtree.sh \
  --prefix lib/awesome-lib \
  --url https://github.com/awesome/lib.git
```

### Workflow 3: Optimize Large Mono-repo with Scalar

```bash
# Register with Scalar
./scripts/mono-repo/scalar/register.sh

# Check status
./scripts/mono-repo/scalar/status.sh

# Measure performance improvement
time git status  # Should be 5-10x faster

# Run manual optimization
./scripts/mono-repo/scalar/optimize.sh
```

### Workflow 4: Migrate from Perforce to Git

```bash
# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Work in Git
git checkout -b feature
# ... make changes ...
git commit -m "Add feature"

# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Eventually migrate fully to Git
./scripts/vcs-bridges/p4/strip-metadata.sh --all
```

---

## Getting Help

### Script Help

All scripts have comprehensive `--help`:

```bash
./scripts/worktree/create-worktree.sh --help
./scripts/subtree/add-subtree.sh --help
./scripts/mono-repo/scalar/register.sh --help
```

### Dry Run

All scripts support `--dry-run`:

```bash
./scripts/worktree/create-worktree.sh feature --dry-run
./scripts/subtree/add-subtree.sh --prefix lib/mylib --url <url> --dry-run
./scripts/mono-repo/scalar/register.sh --dry-run
```

### Documentation

See the comprehensive guides in the `docs/` directory for detailed information.

---

## Requirements

- Git 2.x or higher
- Bash 4.x or higher
- Python 3.x (for git-p4 operations)
- Git Scalar (optional, for mono-repo optimization)
- git-p4 (for Perforce integration)
- git-svn (for Subversion integration)

---

## Contributing

All scripts follow these conventions:
- `set -euo pipefail` (strict mode)
- Comprehensive `--help` text
- `--dry-run` support
- Clear error messages
- Cross-platform compatible (Unix + Git Bash on Windows)

---

## License

Same as kano-git-master-skill project.

---

**Last Updated**: 2026-02-12
**Version**: 2.0
**Total Scripts**: 32
**Total Documentation**: 15 guides
