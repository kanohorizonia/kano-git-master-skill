# Git-SVN Bridge Guide

**Feature**: Subversion integration via git-svn
**Phase**: 7
**Status**: Complete

## Overview

Git-SVN is a bridge that allows you to use Git as a client for Subversion (SVN) repositories. Work with Git locally while synchronizing with SVN servers.

## Scripts

### Core Operations

1. **svn/clone.sh** - Clone from Subversion
2. **svn/fetch.sh** - Fetch from Subversion
3. **svn/dcommit.sh** - Commit to Subversion
4. **svn/rebase.sh** - Rebase with Subversion

## Features

### Subversion Integration
- Clone entire SVN repository to Git
- Support for standard layout (trunk/branches/tags)
- Support for custom layouts
- Fetch changes from SVN
- Commit Git changes to SVN
- Rebase on SVN changes

### Layout Support

**Standard Layout**:
- trunk → main branch
- branches/* → Git branches
- tags/* → Git tags

**Custom Layout**:
- Specify custom paths
- Non-standard directory structure
- Flexible mapping

**Non-Standard Layout**:
- Single branch (no trunk/branches/tags)
- Direct path cloning
- Simple repositories

### Bidirectional Sync
- Import SVN revisions as Git commits
- Export Git commits as SVN revisions
- Maintain metadata for tracking
- Preserve history

## Usage Examples

### Clone from Subversion

```bash
# Clone with standard layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Clone to specific directory
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo my-project

# Clone with custom layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
    --trunk main --branches feature --tags releases

# Clone without standard layout (single branch)
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo/trunk --no-standard

# Dry-run mode
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo --dry-run
```

### Fetch from Subversion

```bash
# Fetch changes from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Dry-run mode
./scripts/vcs-bridges/svn/fetch.sh --dry-run
```

### Rebase with Subversion

```bash
# Rebase on SVN changes
./scripts/vcs-bridges/svn/rebase.sh

# Dry-run mode
./scripts/vcs-bridges/svn/rebase.sh --dry-run
```

### Commit to Subversion

```bash
# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh

# Dry-run mode (shows what would be committed)
./scripts/vcs-bridges/svn/dcommit.sh --dry-run
```

## Use Cases

### SVN Migration

**Gradual Migration**:
- Start using Git locally
- Continue syncing with SVN
- Migrate teams incrementally
- Maintain both systems during transition

**Benefits**:
- Modern Git workflow
- Preserve SVN history
- Bidirectional synchronization
- Smooth transition

### Legacy Systems

**Working with Existing SVN**:
- Work with existing SVN repositories
- Modern Git workflow with SVN backend
- Team collaboration across VCS
- Maintain compatibility

**Benefits**:
- Better local workflow
- Offline commits
- Branching and merging
- Git tools and features

### Hybrid Workflows

**Mixed Teams**:
- Some teams use SVN
- Others use Git
- Share code between systems
- Unified development process

**Benefits**:
- Team flexibility
- Gradual adoption
- Maintain compatibility
- Reduce friction

## Requirements

### System Requirements

- **Git**: 2.x or higher
- **git-svn**: Installed and in PATH
- **Subversion**: Access to SVN server
- **Perl**: Required by git-svn

### Checking Requirements

```bash
# Check git-svn availability
git svn --version

# Check SVN client
svn --version

# Test SVN connection
svn info https://svn.example.com/repo
```

## Configuration

### Initial Setup

```bash
# Clone repository
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Configure Git
cd repo
git config user.name "Your Name"
git config user.email "your.email@example.com"
```

### Git-SVN Configuration

```bash
# Set SVN username
git config svn.username your-username

# Set authors file (for better commit attribution)
git config svn.authorsfile authors.txt

# Ignore SVN properties
git config svn.rmdir true
```

### Authors File

Create `authors.txt` for better commit attribution:

```
svnuser1 = John Doe <john@example.com>
svnuser2 = Jane Smith <jane@example.com>
```

## Best Practices

### Cloning

1. **Use standard layout**: When possible, use standard trunk/branches/tags
2. **Set directory name**: Use meaningful directory names
3. **Verify connection**: Test SVN connection first

### Fetching

2. **Fetch regularly**: Keep in sync with SVN
3. **Rebase after fetch**: Rebase local changes
4. **Resolve conflicts**: Handle conflicts promptly

### Committing

3. **Review before dcommit**: Check changes carefully
4. **Write good messages**: Clear commit messages
5. **Test before dcommit**: Ensure changes work

### Rebasing

4. **Rebase before dcommit**: Always rebase before committing to SVN
5. **Handle conflicts**: Resolve conflicts carefully
6. **Test after rebase**: Verify everything works

## Common Workflows

### Initial Clone and Setup

```bash
# Clone from SVN
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo myproject

# Configure Git
cd myproject
git config user.name "Your Name"
git config user.email "your.email@example.com"

# Set SVN username
git config svn.username your-svn-username
```

### Daily Development

```bash
# Fetch from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Rebase on SVN changes
./scripts/vcs-bridges/svn/rebase.sh

# Make changes
git add .
git commit -m "My changes"

# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh
```

### Working with Branches

```bash
# Fetch all branches
./scripts/vcs-bridges/svn/fetch.sh

# List remote branches
git branch -r

# Create local branch from SVN branch
git checkout -b feature origin/feature

# Work on branch
git add .
git commit -m "Feature work"

# Commit to SVN branch
./scripts/vcs-bridges/svn/dcommit.sh
```

### Conflict Resolution

```bash
# Rebase on SVN
./scripts/vcs-bridges/svn/rebase.sh

# If conflicts occur
git status
# Resolve conflicts in files
git add resolved-files
git rebase --continue

# Commit after resolution
./scripts/vcs-bridges/svn/dcommit.sh
```

## Troubleshooting

### git-svn Not Found

**Problem**: `git svn` command not found

**Solution**:
```bash
# Check if git-svn is installed
git svn --version

# Install git-svn
# On macOS: brew install git
# On Ubuntu: sudo apt install git-svn
# On Windows: Included with Git for Windows

# Verify installation
git svn --version
```

### SVN Connection Failed

**Problem**: Cannot connect to SVN server

**Solution**:
```bash
# Test SVN connection
svn info https://svn.example.com/repo

# Check credentials
svn list https://svn.example.com/repo

# Set username
git config svn.username your-username
```

### Clone Failed

**Problem**: Clone fails with layout errors

**Solution**:
```bash
# Try without standard layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo/trunk --no-standard

# Or specify custom layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
    --trunk main --branches feature --tags releases
```

### Dcommit Failed

**Problem**: Cannot commit to SVN

**Solution**:
```bash
# Check for uncommitted changes
git status

# Rebase before dcommit
./scripts/vcs-bridges/svn/rebase.sh

# Try dcommit again
./scripts/vcs-bridges/svn/dcommit.sh
```

### Rebase Conflicts

**Problem**: Conflicts during rebase

**Solution**:
```bash
# Check conflict status
git status

# Resolve conflicts in files
# Edit conflicted files
git add resolved-files

# Continue rebase
git rebase --continue

# Or abort if needed
git rebase --abort
```

## SVN Layouts

### Standard Layout

```
repo/
├── trunk/          → main branch
├── branches/       → Git branches
│   ├── feature1/
│   └── feature2/
└── tags/           → Git tags
    ├── v1.0/
    └── v2.0/
```

**Clone command**:
```bash
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo
```

### Custom Layout

```
repo/
├── main/           → main branch
├── feature/        → Git branches
│   ├── feature1/
│   └── feature2/
└── releases/       → Git tags
    ├── v1.0/
    └── v2.0/
```

**Clone command**:
```bash
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
    --trunk main --branches feature --tags releases
```

### Non-Standard Layout

```
repo/               → single branch
├── src/
├── docs/
└── tests/
```

**Clone command**:
```bash
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo --no-standard
```

## Metadata Format

### Git-SVN Metadata

Git-SVN adds metadata to commit messages:

```
git-svn-id: https://svn.example.com/repo/trunk@12345 uuid
```

### Extracting Metadata

```bash
# Extract SVN revision
git log --format=%B | grep 'git-svn-id' | sed 's/.*@\([0-9]*\).*/\1/'

# Extract SVN URL
git log --format=%B | grep 'git-svn-id' | sed 's/git-svn-id: \(.*\)@.*/\1/'
```

## Comparison: Git-SVN vs Git-P4

See [Git-P4 vs Git-SVN Comparison](../comparisons/git-p4-vs-git-svn.md) for detailed comparison.

### Quick Comparison

| Feature | Git-SVN | Git-P4 |
|---------|---------|--------|
| VCS | Subversion | Perforce |
| Python | Not required | 3.x required |
| Metadata | Yes | Yes |
| Performance | Good | Good |
| Maturity | Very mature | Mature |

## See Also

- [Git-P4 vs Git-SVN Comparison](../comparisons/git-p4-vs-git-svn.md)
- [SVN Migration Guide](../migrations/svn-to-git.md)
- [Git-P4 Guide](./git-p4.md)
- [Git-SVN Documentation](https://git-scm.com/docs/git-svn)

---

**Last Updated**: 2026-02-12
**Status**: Complete
**Scripts**: 4
