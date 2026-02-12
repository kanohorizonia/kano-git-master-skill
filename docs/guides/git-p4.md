# Git-P4 Bridge Guide

**Feature**: Perforce integration via git-p4
**Phase**: 6
**Status**: Complete

## Overview

Git-P4 is a bridge that allows you to use Git as a client for Perforce (P4) repositories. Work with Git locally while synchronizing with Perforce servers.

**Important**: This implementation requires Python 3. Python 2 is not supported.

## Scripts

### Core Operations

1. **lib/p4-helpers.sh** - Helper library for git-p4
2. **p4/clone.sh** - Clone from Perforce
3. **p4/sync.sh** - Sync from Perforce
4. **p4/submit.sh** - Submit to Perforce
5. **p4/rebase.sh** - Rebase with Perforce

## Features

### Python 3 Validation
- Automatic Python 3 detection
- Clear error messages if Python 3 not found
- Graceful fallback handling
- No Python 2 support

### Perforce Integration
- Clone entire depot to Git
- Sync changes from Perforce
- Submit Git commits to Perforce
- Rebase on Perforce changes

### Metadata Management
- Extract git-p4 metadata from commits
- Strip metadata for cherry-picked commits
- Preserve depot paths and change numbers
- Clean history for main branches

### Environment Support
- P4PORT, P4USER, P4CLIENT variables
- Configuration display
- Validation and error handling
- Cross-platform compatibility

## Usage Examples

### Clone from Perforce

```bash
# Clone entire depot path
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Clone to specific directory
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... my-project

# Clone to specific branch
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... my-project --branch main

# Dry-run mode
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... --dry-run
```

### Sync from Perforce

```bash
# Sync changes from Perforce
./scripts/vcs-bridges/p4/sync.sh

# Sync and rebase
./scripts/vcs-bridges/p4/sync.sh --rebase

# Dry-run mode
./scripts/vcs-bridges/p4/sync.sh --dry-run
```

### Submit to Perforce

```bash
# Submit commits to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Dry-run mode (shows what would be submitted)
./scripts/vcs-bridges/p4/submit.sh --dry-run
```

### Rebase with Perforce

```bash
# Rebase on Perforce changes
./scripts/vcs-bridges/p4/rebase.sh

# Dry-run mode
./scripts/vcs-bridges/p4/rebase.sh --dry-run
```

## Use Cases

### Perforce Migration

**Gradual Migration**:
- Start using Git locally
- Continue syncing with Perforce
- Migrate teams incrementally
- Maintain both systems during transition

**Benefits**:
- Modern Git workflow
- Preserve Perforce history
- Bidirectional synchronization
- Smooth transition

### Multi-Branch Workflows

**Cherry-Pick Between Branches**:
- Cherry-pick commits between P4-synced branches
- Strip metadata before pushing to main
- Maintain clean Git history
- Avoid metadata conflicts

**Workflow**:
```bash
# Cherry-pick from feature branch
git cherry-pick abc123

# Strip git-p4 metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD

# Push to main
git push origin main
```

### Hybrid Environments

**Mixed Teams**:
- Some teams use Perforce
- Others use Git
- Share code between systems
- Unified workflow

**Benefits**:
- Team flexibility
- Gradual adoption
- Maintain compatibility
- Reduce friction

## Requirements

### System Requirements

- **Python**: 3.x (required, Python 2 not supported)
- **Git**: 2.x or higher
- **git-p4**: Installed and in PATH
- **Perforce**: Access to P4 server

### Environment Variables

```bash
# Required
export P4PORT=perforce.example.com:1666
export P4USER=your-username
export P4CLIENT=your-client-workspace

# Optional
export P4PASSWD=your-password  # Or use P4 tickets
```

### Checking Requirements

```bash
# Check Python version
python3 --version

# Check git-p4 availability
git p4 --version

# Check Perforce connection
p4 info
```

## Configuration

### Initial Setup

```bash
# Set Perforce environment
export P4PORT=perforce.example.com:1666
export P4USER=your-username
export P4CLIENT=your-workspace

# Clone repository
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Configure Git
cd myproject
git config user.name "Your Name"
git config user.email "your.email@example.com"
```

### Git-P4 Configuration

```bash
# Set default depot path
git config git-p4.depotPath //depot/myproject/...

# Set client workspace
git config git-p4.client your-workspace

# Enable metadata
git config git-p4.skipSubmitEdit false
```

## Best Practices

### Cloning

1. **Use specific depot paths**: Clone only what you need
2. **Set branch name**: Use meaningful branch names
3. **Verify connection**: Test P4 connection first

### Syncing

2. **Sync regularly**: Keep in sync with Perforce
3. **Rebase after sync**: Rebase local changes
4. **Resolve conflicts**: Handle conflicts promptly

### Submitting

3. **Review before submit**: Check changes carefully
4. **Write good messages**: Clear commit messages
5. **Test before submit**: Ensure changes work

### Metadata

4. **Strip when needed**: Remove metadata for cherry-picks
5. **Preserve for tracking**: Keep metadata for P4-synced branches
6. **Document decisions**: Note when and why stripping

## Common Workflows

### Initial Clone and Setup

```bash
# Set environment
export P4PORT=perforce.example.com:1666
export P4USER=your-username
export P4CLIENT=your-workspace

# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... myproject

# Configure Git
cd myproject
git config user.name "Your Name"
git config user.email "your.email@example.com"
```

### Daily Development

```bash
# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Make changes
git add .
git commit -m "My changes"

# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh
```

### Cherry-Pick Workflow

```bash
# Cherry-pick from feature branch
git checkout main
git cherry-pick feature-branch~3..feature-branch

# Strip git-p4 metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~3..HEAD

# Push to Git remote
git push origin main
```

### Conflict Resolution

```bash
# Sync and rebase
./scripts/vcs-bridges/p4/rebase.sh

# If conflicts occur
git status
# Resolve conflicts in files
git add resolved-files
git rebase --continue

# Submit after resolution
./scripts/vcs-bridges/p4/submit.sh
```

## Troubleshooting

### Python 3 Not Found

**Problem**: Script reports Python 3 not available

**Solution**:
```bash
# Check Python version
python3 --version

# Install Python 3
# On macOS: brew install python3
# On Ubuntu: sudo apt install python3
# On Windows: Download from python.org

# Verify installation
python3 --version
```

### git-p4 Not Found

**Problem**: `git p4` command not found

**Solution**:
```bash
# Check if git-p4 is installed
git p4 --version

# Install git-p4
# On macOS: brew install git
# On Ubuntu: sudo apt install git-p4
# On Windows: Included with Git for Windows

# Verify installation
git p4 --version
```

### Perforce Connection Failed

**Problem**: Cannot connect to Perforce server

**Solution**:
```bash
# Check environment variables
echo $P4PORT
echo $P4USER
echo $P4CLIENT

# Test connection
p4 info

# Set variables if missing
export P4PORT=perforce.example.com:1666
export P4USER=your-username
export P4CLIENT=your-workspace
```

### Submit Failed

**Problem**: Cannot submit to Perforce

**Solution**:
```bash
# Check for uncommitted changes
git status

# Check Perforce workspace
p4 client -o

# Sync before submit
./scripts/vcs-bridges/p4/sync.sh --rebase

# Try submit again
./scripts/vcs-bridges/p4/submit.sh
```

### Metadata Issues

**Problem**: git-p4 metadata causing problems

**Solution**:
```bash
# Strip metadata from commits
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~5..HEAD

# Or strip from specific range
./scripts/vcs-bridges/p4/strip-metadata.sh abc123..def456

# Verify metadata removed
git log --format=full
```

## Metadata Format

### Git-P4 Metadata

Git-P4 adds metadata to commit messages:

```
[git-p4: depot-paths = "//depot/myproject/": change = 12345]
```

### Extracting Metadata

```bash
# Extract depot path
git log --format=%B | grep 'git-p4: depot-paths'

# Extract change number
git log --format=%B | grep 'change =' | sed 's/.*change = \([0-9]*\).*/\1/'
```

### When to Strip Metadata

**Strip metadata when**:
- Cherry-picking between branches
- Pushing to pure Git remotes
- Creating clean history
- Avoiding metadata conflicts

**Keep metadata when**:
- Working on P4-synced branches
- Need to track P4 changes
- Bidirectional sync required
- Debugging P4 issues

## Comparison: Git-P4 vs Git-SVN

See [Git-P4 vs Git-SVN Comparison](../comparisons/git-p4-vs-git-svn.md) for detailed comparison.

### Quick Comparison

| Feature | Git-P4 | Git-SVN |
|---------|--------|---------|
| VCS | Perforce | Subversion |
| Python | 3.x required | Not required |
| Metadata | Yes | Yes |
| Performance | Good | Good |
| Maturity | Mature | Very mature |

## See Also

- [Git-P4 vs Git-SVN Comparison](../comparisons/git-p4-vs-git-svn.md)
- [Perforce Migration Guide](../migrations/perforce-to-git.md)
- [Git-SVN Guide](./git-svn.md)
- [Git-P4 Documentation](https://git-scm.com/docs/git-p4)

---

**Last Updated**: 2026-02-12
**Status**: Complete
**Scripts**: 5
**Python**: 3.x required
