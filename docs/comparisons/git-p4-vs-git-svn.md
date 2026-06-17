# Git-P4 vs Git-SVN - Comparison Guide

**Last Updated**: 2026-02-12

## Overview

Both git-p4 and git-svn are bridges that allow you to use Git with centralized version control systems (Perforce and Subversion). They enable bidirectional synchronization between Git and the centralized VCS.

## Quick Comparison

| Feature | Git-P4 | Git-SVN |
|---------|--------|---------|
| **Target VCS** | Perforce | Subversion |
| **Complexity** | Higher | Medium |
| **Performance** | Faster | Slower |
| **Python Required** | Yes (Python 3) | No |
| **Metadata** | Verbose | Compact |
| **Branch Support** | Limited | Good |
| **Tag Support** | No | Yes |
| **Maturity** | Mature | Very Mature |
| **Community** | Smaller | Larger |

## Git-P4 (Perforce Bridge)

### What is Git-P4?

Git-p4 is a bidirectional bridge between Git and Perforce. It allows you to clone Perforce depots as Git repositories, work with Git locally, and submit changes back to Perforce.

### Advantages

1. **Fast Operations**: Perforce is generally faster than SVN
2. **Better for Large Files**: Perforce handles large binary files well
3. **Stream Support**: Supports Perforce streams
4. **Change Numbers**: Preserves Perforce change numbers
5. **Depot Paths**: Tracks depot paths in metadata

### Disadvantages

1. **Python 3 Required**: Must have Python 3 installed
2. **Complex Metadata**: Verbose metadata in commit messages
3. **Limited Branch Support**: Perforce branches are complex
4. **No Tag Support**: Perforce doesn't have native tags
5. **Smaller Community**: Fewer users than git-svn

### Requirements

- Python 3.x (Python 2 not supported)
- git-p4 installed
- P4PORT environment variable
- Perforce credentials

### Typical Workflow

```bash
# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Make changes in Git
git checkout -b feature-branch
# ... make changes ...
git commit -m "Add feature"

# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh
```

## Git-SVN (Subversion Bridge)

### What is Git-SVN?

Git-svn is a bidirectional bridge between Git and Subversion. It allows you to clone SVN repositories as Git repositories, work with Git locally, and commit changes back to SVN.

### Advantages

1. **No Python Required**: Built into Git
2. **Compact Metadata**: Less verbose metadata
3. **Good Branch Support**: SVN branches map well to Git
4. **Tag Support**: SVN tags map to Git tags
5. **Large Community**: Many users and resources
6. **Mature**: Very stable and well-tested

### Disadvantages

1. **Slower Operations**: SVN is generally slower than Perforce
2. **Poor Large File Support**: SVN struggles with large binaries
3. **Complex History**: SVN history can be messy
4. **Revision Numbers**: SVN revision numbers don't map cleanly

### Requirements

- git-svn installed
- Subversion credentials
- SVN repository access

### Typical Workflow

```bash
# Clone from Subversion
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Make changes in Git
git checkout -b feature-branch
# ... make changes ...
git commit -m "Add feature"

# Rebase on SVN
./scripts/vcs-bridges/svn/rebase.sh

# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh
```

## Detailed Comparison

### Clone Operation

**Git-P4**:
```bash
# Clone entire depot
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Clone to specific directory
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... my-project

# Clone to specific branch
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/... --branch main
```

**Git-SVN**:
```bash
# Clone with standard layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Clone with custom layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
    --trunk main --branches feature --tags releases

# Clone without standard layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo/trunk --no-standard
```

### Sync/Fetch Operation

**Git-P4**:
```bash
# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh

# Sync and rebase
./scripts/vcs-bridges/p4/sync.sh --rebase
```

**Git-SVN**:
```bash
# Fetch from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Fetch is separate from rebase
```

### Submit/Commit Operation

**Git-P4**:
```bash
# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Converts Git commits to Perforce changelists
```

**Git-SVN**:
```bash
# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh

# Converts Git commits to SVN revisions
```

### Rebase Operation

**Git-P4**:
```bash
# Rebase on Perforce changes
./scripts/vcs-bridges/p4/rebase.sh

# Equivalent to sync --rebase
```

**Git-SVN**:
```bash
# Rebase on SVN changes
./scripts/vcs-bridges/svn/rebase.sh

# Equivalent to fetch + rebase
```

### Metadata Format

**Git-P4 Metadata**:
```
[git-p4: depot-paths = "//depot/myproject/": change = 12345]
```
- Verbose and detailed
- Includes depot path and change number
- Can be stripped with strip-metadata.sh

**Git-SVN Metadata**:
```
git-svn-id: https://svn.example.com/repo/trunk@12345 uuid
```
- Compact and simple
- Includes SVN URL and revision number
- Harder to strip

### Branch Handling

**Git-P4**:
- Perforce branches are complex (depot paths)
- Limited automatic branch detection
- Manual branch mapping often needed
- Streams provide better branch support

**Git-SVN**:
- SVN branches map well to Git branches
- Automatic branch detection with standard layout
- Custom layout support for non-standard repos
- Tags map to Git tags

### Performance

**Git-P4**:
- Generally faster operations
- Perforce server is optimized for performance
- Better for large repositories
- Good for large binary files

**Git-SVN**:
- Generally slower operations
- SVN server can be slow for large repos
- Struggles with large binary files
- Better for text-heavy repositories

## Use Case Comparison

### When to Use Git-P4

1. **Perforce Environment**
   - Your company uses Perforce
   - You want Git workflow with Perforce backend

2. **Large Binary Files**
   - Repository has many large binary files
   - Perforce handles binaries better than SVN

3. **Performance Critical**
   - Need fast operations
   - Large repository with many files

4. **Stream-Based Workflow**
   - Using Perforce streams
   - Need stream support

### When to Use Git-SVN

1. **Subversion Environment**
   - Your company uses Subversion
   - You want Git workflow with SVN backend

2. **Text-Heavy Repository**
   - Repository is mostly text files
   - Few or no binary files

3. **Standard Layout**
   - SVN repo uses trunk/branches/tags layout
   - Want automatic branch/tag mapping

4. **Mature Tooling**
   - Need stable, well-tested tools
   - Want large community support

## Migration Scenarios

### Perforce to Git (via Git-P4)

```bash
# 1. Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# 2. Work in Git
cd myproject
git checkout -b feature
# ... make changes ...
git commit -m "Add feature"

# 3. Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# 4. Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh

# 5. Eventually migrate fully to Git
# - Stop using Perforce
# - Strip git-p4 metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~10..HEAD
```

### Subversion to Git (via Git-SVN)

```bash
# 1. Clone from SVN
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# 2. Work in Git
cd repo
git checkout -b feature
# ... make changes ...
git commit -m "Add feature"

# 3. Rebase on SVN
./scripts/vcs-bridges/svn/rebase.sh

# 4. Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh

# 5. Eventually migrate fully to Git
# - Stop using SVN
# - git-svn metadata remains in history
```

## Hybrid Workflows

### Git-P4 Hybrid

**Scenario**: Some developers use Git, others use Perforce

```bash
# Git developers
git clone <git-repo>
# ... work in Git ...
git push

# CI/CD syncs Git to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Perforce developers
p4 sync
# ... work in Perforce ...
p4 submit

# CI/CD syncs Perforce to Git
./scripts/vcs-bridges/p4/sync.sh
git push
```

### Git-SVN Hybrid

**Scenario**: Some developers use Git, others use SVN

```bash
# Git developers
git clone <git-repo>
# ... work in Git ...
git push

# CI/CD syncs Git to SVN
./scripts/vcs-bridges/svn/dcommit.sh

# SVN developers
svn checkout <svn-repo>
# ... work in SVN ...
svn commit

# CI/CD syncs SVN to Git
./scripts/vcs-bridges/svn/fetch.sh
git push
```

## Troubleshooting

### Git-P4 Issues

**Python 3 Not Found**:
```bash
# Check Python version
python3 --version

# Install Python 3
# - Windows: python.org
# - macOS: brew install python3
# - Linux: apt-get install python3
```

**P4PORT Not Set**:
```bash
# Set P4PORT
export P4PORT=perforce:1666

# Add to shell profile
echo 'export P4PORT=perforce:1666' >> ~/.bashrc
```

**Metadata in Commits**:
```bash
# Strip git-p4 metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~10..HEAD
```

### Git-SVN Issues

**Slow Operations**:
```bash
# SVN is inherently slower
# Consider migrating to Git fully

# Or use shallow clone
git svn clone --depth 1 <svn-url>
```

**Branch Detection Failed**:
```bash
# Use custom layout
./scripts/vcs-bridges/svn/clone.sh <svn-url> \
    --trunk main --branches feature --tags releases
```

**Large Repository**:
```bash
# Clone specific path only
./scripts/vcs-bridges/svn/clone.sh <svn-url>/trunk --no-standard
```

## Best Practices

### Git-P4

1. **Use Python 3**: Always use Python 3, not Python 2
2. **Strip Metadata**: Strip metadata before pushing to pure Git repos
3. **Sync Frequently**: Sync from Perforce regularly to avoid conflicts
4. **Test Locally**: Test changes locally before submitting
5. **Document Workflow**: Add README with git-p4 instructions

### Git-SVN

1. **Use Standard Layout**: Use trunk/branches/tags when possible
2. **Rebase Before Dcommit**: Always rebase before committing to SVN
3. **Avoid Merge Commits**: SVN doesn't handle merge commits well
4. **Linear History**: Keep history linear for SVN compatibility
5. **Document Workflow**: Add README with git-svn instructions

## Scripts Reference

### Git-P4 Scripts

```bash
# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Submit to Perforce
./scripts/vcs-bridges/p4/submit.sh

# Rebase on Perforce
./scripts/vcs-bridges/p4/rebase.sh

# Strip metadata
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~10..HEAD
```

### Git-SVN Scripts

```bash
# Clone from SVN
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Fetch from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Rebase on SVN
./scripts/vcs-bridges/svn/rebase.sh

# Commit to SVN
./scripts/vcs-bridges/svn/dcommit.sh
```

## Conclusion

**Use Git-P4 if**:
- You're working with Perforce
- You need fast operations
- You have large binary files
- You're using Perforce streams

**Use Git-SVN if**:
- You're working with Subversion
- You have text-heavy repository
- You want mature, stable tooling
- You need good branch/tag support

**General Recommendation**: Both tools enable Git workflow with centralized VCS. Choose based on your existing VCS (Perforce or SVN). Consider migrating fully to Git when possible.

---

**See Also**:
- [Git-P4 Guide](../guides/git-p4.md)
- [Git-SVN Guide](../guides/git-svn.md)
- [Perforce Migration Guide](../migrations/perforce-to-git.md)
- [SVN Migration Guide](../migrations/svn-to-git.md)
