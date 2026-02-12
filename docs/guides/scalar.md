# Git Scalar - Mono-repo Optimization Guide

**Feature**: Git Scalar integration for large repositories
**Phase**: 5
**Status**: Complete

## Overview

Git Scalar is a tool for managing large Git repositories (mono-repos). It provides automatic optimizations that can improve performance by 10-20x for large repositories.

## What is Git Scalar?

Git Scalar is a repository management tool that:
- Configures Git for optimal performance on large repos
- Enables partial clone (download only what you need)
- Sets up sparse checkout (check out only what you need)
- Runs background maintenance automatically
- Monitors file system changes for faster status

## Scripts

### Core Operations

1. **scalar/register.sh** - Register repo with Scalar
2. **scalar/status.sh** - Show Scalar status
3. **scalar/optimize.sh** - Run optimizations manually
4. **scalar/unregister.sh** - Unregister from Scalar

## Features

### Automatic Optimizations

**Partial Clone** (blob:none filter):
- Downloads only commit and tree objects initially
- Fetches blobs (file contents) on demand
- Reduces initial clone time by 10-20x

**Sparse Checkout** (cone mode):
- Checks out only specified directories
- Reduces working tree size
- Faster checkout and status operations

**Background Maintenance**:
- Hourly: prefetch, commit-graph, loose-objects
- Daily: incremental-repack
- Weekly: pack-refs
- Runs automatically in background

**FSMonitor** (File System Monitor):
- Watches file system for changes
- Speeds up `git status` by 5-10x
- Reduces disk I/O

**Multi-Pack Index**:
- Improves object lookup performance
- Reduces memory usage
- Faster git operations

**Commit Graph**:
- Speeds up commit traversal
- Faster git log, git blame
- Improves merge performance

### Performance Improvements

Typical improvements on large repos:
- **Initial clone**: 10-20x faster
- **git status**: 5-10x faster
- **git checkout**: 3-5x faster
- **Disk usage**: 50-90% reduction

### Status Monitoring

**Text Output**:
- Configuration display
- Repository statistics
- Maintenance schedule

**JSON Output**:
- Machine-readable format
- Integration with tools
- Automated monitoring

### Manual Optimization

**Prefetch**:
- Fetch new objects from remotes
- Update commit graph
- Prepare for offline work

**Commit Graph**:
- Update commit graph file
- Improve traversal performance
- Faster log and blame

**Loose Objects**:
- Pack loose objects
- Reduce file count
- Improve performance

**Incremental Repack**:
- Repack objects incrementally
- Reduce disk usage
- Maintain performance

**Pack References**:
- Pack reference files
- Reduce file count
- Faster reference operations

## Usage Examples

### Register with Scalar

```bash
# Register current repository
./scripts/mono-repo/scalar/register.sh

# Dry-run mode
./scripts/mono-repo/scalar/register.sh --dry-run
```

### Check Status

```bash
# Show status (text format)
./scripts/mono-repo/scalar/status.sh

# Show status (JSON format)
./scripts/mono-repo/scalar/status.sh --format json

# Show detailed statistics
./scripts/mono-repo/scalar/status.sh --verbose
```

### Run Optimizations

```bash
# Run all optimizations
./scripts/mono-repo/scalar/optimize.sh

# Run specific optimization
./scripts/mono-repo/scalar/optimize.sh --prefetch
./scripts/mono-repo/scalar/optimize.sh --commit-graph
./scripts/mono-repo/scalar/optimize.sh --loose-objects
./scripts/mono-repo/scalar/optimize.sh --incremental-repack
./scripts/mono-repo/scalar/optimize.sh --pack-refs

# Dry-run mode
./scripts/mono-repo/scalar/optimize.sh --dry-run
```

### Unregister

```bash
# Unregister from Scalar
./scripts/mono-repo/scalar/unregister.sh

# Dry-run mode
./scripts/mono-repo/scalar/unregister.sh --dry-run
```

## Use Cases

### Large Mono-repos

**When to use**:
- Repository size > 1GB
- File count > 100k files
- Multiple projects in one repo
- Shared libraries and dependencies

**Benefits**:
- Faster clone and checkout
- Reduced disk usage
- Better performance
- Automatic maintenance

### Performance Critical Environments

**CI/CD Pipelines**:
- Faster builds
- Reduced clone time
- Lower resource usage

**Developer Workstations**:
- Faster git operations
- Better responsiveness
- Reduced disk I/O

**Build Servers**:
- Faster builds
- Reduced disk usage
- Better scalability

**Automated Testing**:
- Faster test runs
- Reduced setup time
- Better resource utilization

## Decision Guide

See [When to Use Scalar](../comparisons/when-to-use-scalar.md) for detailed decision guide.

### Should You Use Scalar?

**Use Scalar if**:
- Repository > 1GB or > 100k files
- Clone time > 5 minutes
- `git status` takes > 1 second
- Disk space is limited
- You work on a subset of the repo

**Don't use Scalar if**:
- Repository < 100MB
- Fast operations already
- Need full repository history
- Working with all files

## Requirements

### System Requirements

- **Git Version**: 2.38 or higher (with Scalar support)
- **Operating System**: Windows, macOS, Linux
- **Disk Space**: Sufficient for partial clone
- **Network**: For background maintenance

### Checking Scalar Availability

```bash
# Check if Scalar is available
git scalar --version

# Check Git version
git --version
```

## Configuration

### Scalar Configuration

Scalar configures the following Git settings:

```bash
# Partial clone
core.fsmonitor=true
core.untrackedCache=true
core.multiPackIndex=true

# Sparse checkout
core.sparseCheckout=true
core.sparseCheckoutCone=true

# Background maintenance
maintenance.auto=true
maintenance.strategy=incremental

# Commit graph
commitGraph.generationVersion=2
commitGraph.readChangedPaths=true

# Fetch
fetch.parallel=0
fetch.writeCommitGraph=true
```

### Customizing Configuration

```bash
# Disable FSMonitor
git config core.fsmonitor false

# Change maintenance schedule
git config maintenance.hourly.enabled false

# Adjust sparse checkout
git sparse-checkout set path/to/dir
```

## Best Practices

### Registration

1. **Register early**: Register immediately after clone
2. **Verify status**: Check status after registration
3. **Monitor performance**: Track improvements

### Maintenance

2. **Let it run**: Allow background maintenance to run
3. **Manual optimization**: Run optimize script before important work
4. **Monitor disk usage**: Check disk usage periodically

### Sparse Checkout

3. **Start small**: Check out only what you need
4. **Expand gradually**: Add directories as needed
5. **Update regularly**: Update sparse checkout patterns

### Monitoring

4. **Check status**: Run status script regularly
5. **Review logs**: Check maintenance logs
6. **Track metrics**: Monitor performance metrics

## Common Workflows

### Initial Setup

```bash
# Clone repository
git clone https://github.com/user/large-repo.git
cd large-repo

# Register with Scalar
./scripts/mono-repo/scalar/register.sh

# Check status
./scripts/mono-repo/scalar/status.sh

# Set sparse checkout (optional)
git sparse-checkout set src/myproject
```

### Daily Development

```bash
# Check status
./scripts/mono-repo/scalar/status.sh

# Work normally
git status
git add .
git commit -m "Changes"
git push

# Background maintenance runs automatically
```

### Before Important Work

```bash
# Run optimizations
./scripts/mono-repo/scalar/optimize.sh

# Verify status
./scripts/mono-repo/scalar/status.sh

# Proceed with work
```

### Troubleshooting

```bash
# Check status
./scripts/mono-repo/scalar/status.sh --verbose

# Run optimizations
./scripts/mono-repo/scalar/optimize.sh

# If issues persist, unregister and re-register
./scripts/mono-repo/scalar/unregister.sh
./scripts/mono-repo/scalar/register.sh
```

## Troubleshooting

### Scalar Not Available

**Problem**: `git scalar` command not found

**Solution**:
```bash
# Check Git version
git --version

# Upgrade Git to 2.38 or higher
# On macOS: brew upgrade git
# On Ubuntu: sudo apt update && sudo apt upgrade git
# On Windows: Download from git-scm.com
```

### Background Maintenance Not Running

**Problem**: Maintenance tasks not executing

**Solution**:
```bash
# Check maintenance configuration
git config --get-regexp maintenance

# Enable maintenance
git config maintenance.auto true

# Run manually
./scripts/mono-repo/scalar/optimize.sh
```

### FSMonitor Issues

**Problem**: FSMonitor causing problems

**Solution**:
```bash
# Disable FSMonitor
git config core.fsmonitor false

# Re-enable after troubleshooting
git config core.fsmonitor true
```

### Sparse Checkout Issues

**Problem**: Files not appearing in working tree

**Solution**:
```bash
# Check sparse checkout patterns
git sparse-checkout list

# Add missing directories
git sparse-checkout add path/to/dir

# Disable sparse checkout if needed
git sparse-checkout disable
```

## Performance Metrics

### Before Scalar

Typical large repo (5GB, 500k files):
- Clone time: 30-60 minutes
- git status: 5-10 seconds
- git checkout: 10-20 seconds
- Disk usage: 5GB

### After Scalar

Same repo with Scalar:
- Clone time: 2-5 minutes (10-20x faster)
- git status: 0.5-1 second (5-10x faster)
- git checkout: 2-5 seconds (3-5x faster)
- Disk usage: 500MB-1GB (50-90% reduction)

## See Also

- [When to Use Scalar](../comparisons/when-to-use-scalar.md)
- [Worktree Management Guide](./worktree.md)
- [Git Scalar Documentation](https://github.com/microsoft/scalar)

---

**Last Updated**: 2026-02-12
**Status**: Complete
**Scripts**: 4
