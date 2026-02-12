# When to Use Git Scalar - Decision Guide

**Last Updated**: 2026-02-12

## What is Git Scalar?

Git Scalar is a tool for managing large Git repositories (mono-repos). It configures Git with optimizations that can provide 10-20x performance improvements for large repositories.

## Quick Decision

### Use Scalar If:
- ✅ Repository > 1GB
- ✅ Repository > 100,000 files
- ✅ `git status` takes > 5 seconds
- ✅ `git checkout` takes > 10 seconds
- ✅ You have a mono-repo with multiple projects

### Don't Use Scalar If:
- ❌ Repository < 100MB
- ❌ Repository < 10,000 files
- ❌ Git operations are already fast
- ❌ You're using Git < 2.38

## What Scalar Does

### Optimizations Enabled

1. **Partial Clone (blob:none)**
   - Downloads only needed objects, not full history
   - Reduces initial clone time by 10-20x
   - Reduces disk usage by 50-90%

2. **Sparse Checkout (cone mode)**
   - Checks out only needed files
   - Reduces working tree size
   - Speeds up file operations

3. **Background Maintenance**
   - Automatic prefetch (hourly)
   - Automatic commit-graph updates (hourly)
   - Automatic loose object packing (daily)
   - Automatic incremental repack (daily)

4. **FSMonitor (File System Monitor)**
   - Watches file system for changes
   - Speeds up `git status` by 5-10x
   - Reduces CPU usage

5. **Multi-pack Index**
   - Faster object lookups
   - Reduces pack file overhead

6. **Commit Graph**
   - Faster history traversal
   - Speeds up `git log`, `git merge-base`

## Performance Impact

### Before Scalar (Large Repo)

```bash
# Initial clone
git clone https://github.com/large/repo.git
# Time: 30-60 minutes
# Size: 5GB

# git status
git status
# Time: 10-30 seconds

# git checkout
git checkout feature-branch
# Time: 20-40 seconds

# git log
git log --oneline -100
# Time: 5-10 seconds
```

### After Scalar (Same Repo)

```bash
# Initial clone with Scalar
git clone https://github.com/large/repo.git
cd repo
git scalar register
# Time: 3-5 minutes
# Size: 500MB-1GB

# git status
git status
# Time: 1-2 seconds (5-10x faster)

# git checkout
git checkout feature-branch
# Time: 3-5 seconds (5-8x faster)

# git log
git log --oneline -100
# Time: 0.5-1 second (5-10x faster)
```

## Repository Size Thresholds

### Small Repository (< 100MB)
- **Recommendation**: Don't use Scalar
- **Reason**: Overhead > benefits
- **Alternative**: Standard Git is fast enough

### Medium Repository (100MB - 1GB)
- **Recommendation**: Consider Scalar if operations are slow
- **Reason**: Benefits start to appear
- **Test**: Try Scalar and measure improvements

### Large Repository (1GB - 10GB)
- **Recommendation**: Use Scalar
- **Reason**: Significant performance improvements
- **Expected**: 5-10x faster operations

### Very Large Repository (> 10GB)
- **Recommendation**: Definitely use Scalar
- **Reason**: Essential for reasonable performance
- **Expected**: 10-20x faster operations

## File Count Thresholds

### Small Repository (< 10,000 files)
- **Recommendation**: Don't use Scalar
- **Reason**: Standard Git is fast enough

### Medium Repository (10,000 - 100,000 files)
- **Recommendation**: Consider Scalar
- **Reason**: `git status` may be slow

### Large Repository (> 100,000 files)
- **Recommendation**: Use Scalar
- **Reason**: `git status` will be very slow without it

## Use Cases

### Ideal for Scalar

1. **Mono-repos**
   - Multiple projects in one repository
   - Shared libraries and dependencies
   - Example: Google's internal repo, Microsoft's Windows repo

2. **Large Codebases**
   - Enterprise applications
   - Legacy systems
   - Long-running projects

3. **CI/CD Pipelines**
   - Faster clone times
   - Reduced build times
   - Lower infrastructure costs

4. **Developer Workstations**
   - Faster `git status`
   - Faster branch switching
   - Better developer experience

### Not Ideal for Scalar

1. **Small Projects**
   - Personal projects
   - Small libraries
   - Prototypes

2. **Frequently Changing Repos**
   - Repos that change structure often
   - Experimental projects

3. **Simple Workflows**
   - Single-branch workflows
   - Infrequent commits

## How to Enable Scalar

### Prerequisites

```bash
# Check Git version (need 2.38+)
git --version

# Check if Scalar is available
git scalar --help
```

### Enable Scalar

```bash
# Navigate to repository
cd /path/to/repo

# Register with Scalar
./scripts/mono-repo/scalar/register.sh

# Check status
./scripts/mono-repo/scalar/status.sh
```

### Verify Improvements

```bash
# Before: Measure git status time
time git status

# After: Should be 5-10x faster
time git status
```

## Configuration Details

### What Scalar Configures

```bash
# Partial clone
git config remote.origin.promisor true
git config remote.origin.partialclonefilter blob:none

# Sparse checkout
git config core.sparseCheckout true
git config core.sparseCheckoutCone true

# FSMonitor
git config core.fsmonitor true
git config core.untrackedCache true

# Multi-pack index
git config core.multiPackIndex true

# Commit graph
git config core.commitGraph true
git config gc.writeCommitGraph true

# Background maintenance
git config maintenance.auto true
git config maintenance.strategy incremental
```

### Maintenance Schedule

```bash
# Prefetch: Hourly
git config maintenance.prefetch.schedule hourly

# Commit graph: Hourly
git config maintenance.commit-graph.schedule hourly

# Loose objects: Daily
git config maintenance.loose-objects.schedule daily

# Incremental repack: Daily
git config maintenance.incremental-repack.schedule daily
```

## Troubleshooting

### Scalar Not Available

```bash
# Error: git scalar not found
# Solution: Upgrade Git to 2.38+

# Check current version
git --version

# Upgrade Git
# - Windows: Download from git-scm.com
# - macOS: brew upgrade git
# - Linux: apt-get upgrade git or yum upgrade git
```

### Performance Not Improved

```bash
# Check if Scalar is actually enabled
./scripts/mono-repo/scalar/status.sh

# Run manual optimization
./scripts/mono-repo/scalar/optimize.sh

# Check FSMonitor status
git config --get core.fsmonitor

# Restart FSMonitor (if needed)
# - Windows: Restart Git Credential Manager
# - macOS: Restart Watchman
# - Linux: Restart inotify
```

### Disk Usage Increased

```bash
# This is normal during incremental repack
# Run full optimization to reduce size
./scripts/mono-repo/scalar/optimize.sh

# Check repository size
du -sh .git

# Force garbage collection
git gc --aggressive
```

## Comparison with Other Tools

### Scalar vs Standard Git

| Feature | Standard Git | Git Scalar |
|---------|-------------|------------|
| Clone time | Slow | 10-20x faster |
| Disk usage | High | 50-90% less |
| git status | Slow | 5-10x faster |
| git checkout | Slow | 3-5x faster |
| Setup | None | One-time setup |
| Maintenance | Manual | Automatic |

### Scalar vs Git LFS

| Feature | Git LFS | Git Scalar |
|---------|---------|------------|
| Purpose | Large files | Large repos |
| Target | Binary files | All files |
| Server | Requires LFS server | Standard Git |
| Complexity | Medium | Low |
| Performance | Good for binaries | Good for everything |

### Scalar vs Sparse Checkout

| Feature | Sparse Checkout | Git Scalar |
|---------|----------------|------------|
| Scope | Working tree only | Everything |
| Setup | Manual | Automatic |
| Maintenance | Manual | Automatic |
| Performance | Good | Better |

## Best Practices

### When Enabling Scalar

1. **Measure First**: Benchmark before and after
2. **Test on Clone**: Test with fresh clone
3. **Document**: Add README note about Scalar
4. **CI/CD**: Enable Scalar in CI/CD pipelines
5. **Team Training**: Educate team about Scalar

### Ongoing Maintenance

1. **Monitor Performance**: Track git operation times
2. **Run Optimizations**: Periodically run manual optimization
3. **Check Status**: Regularly check Scalar status
4. **Update Git**: Keep Git updated for latest improvements
5. **Review Config**: Periodically review Scalar configuration

### When to Disable Scalar

1. **Repository Shrinks**: If repo becomes small
2. **Performance Issues**: If Scalar causes problems
3. **Team Confusion**: If team struggles with Scalar
4. **Migration**: When migrating to different VCS

## Scripts Reference

### Register with Scalar
```bash
./scripts/mono-repo/scalar/register.sh
```

### Check Status
```bash
./scripts/mono-repo/scalar/status.sh
./scripts/mono-repo/scalar/status.sh --format json
```

### Run Optimizations
```bash
./scripts/mono-repo/scalar/optimize.sh
```

### Unregister
```bash
./scripts/mono-repo/scalar/unregister.sh
```

## Real-World Examples

### Example 1: Microsoft Windows Repository

- **Size**: > 300GB
- **Files**: > 3.5 million
- **Without Scalar**: 12+ hours to clone
- **With Scalar**: 30 minutes to clone
- **Improvement**: 24x faster

### Example 2: Large Enterprise Mono-repo

- **Size**: 50GB
- **Files**: 500,000
- **Without Scalar**: `git status` takes 45 seconds
- **With Scalar**: `git status` takes 3 seconds
- **Improvement**: 15x faster

### Example 3: CI/CD Pipeline

- **Clone time**: 20 minutes → 2 minutes (10x faster)
- **Build time**: 45 minutes → 30 minutes (1.5x faster)
- **Cost savings**: 30% reduction in CI/CD costs

## Conclusion

**Use Git Scalar if**:
- Your repository is > 1GB or > 100,000 files
- Git operations are noticeably slow
- You have a mono-repo
- You want automatic maintenance

**Don't use Git Scalar if**:
- Your repository is small (< 100MB)
- Git operations are already fast
- You're using Git < 2.38

**General Recommendation**: Try Scalar on large repositories and measure the improvements. The benefits usually far outweigh the minimal setup cost.

---

**See Also**:
- [Scalar Scripts Documentation](../scripts/mono-repo/scalar/)
- [Git Scalar Official Docs](https://github.com/microsoft/scalar)
- [Performance Benchmarks](./PERFORMANCE-BENCHMARKS.md) (to be created)
