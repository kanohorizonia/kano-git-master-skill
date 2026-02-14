# Submodule Management Guide

**Feature**: Enhanced Git submodule operations
**Phase**: 4
**Status**: Complete

## Overview

Enhanced submodule management with batch operations, recursive updates, and safe removal. Extends Git's built-in submodule functionality with AI-friendly defaults and comprehensive error handling.

## Scripts

### Core Operations

1. **add-submodule.sh** - Add a new submodule
2. **update-submodules.sh** - Update all submodules
3. **remove-submodule.sh** - Remove submodule safely
4. **foreach-submodule.sh** - Execute command in each submodule

## Features

### Update Submodules
- Update to recorded commits or latest remote commits
- Recursive update (nested submodules)
- Dry-run support
- Progress reporting

### Remove Submodule
- Safe removal following Git best practices
- Removes from .gitmodules, .git/config, and working tree
- Cleans up .git/modules directory
- Validates before removal

### Foreach Submodule
- Execute arbitrary commands in each submodule
- Access to special variables ($name, $path, $sha1, $toplevel)
- Recursive execution support
- Error handling per submodule

## Usage Examples

### Update Submodules

```bash
# Update all submodules to recorded commits
./scripts/submodules/update-submodules.sh

# Update to latest remote commits
./scripts/submodules/update-submodules.sh --remote

# Update recursively (nested submodules)
./scripts/submodules/update-submodules.sh --recursive

# Combine options
./scripts/submodules/update-submodules.sh --remote --recursive

# Dry-run mode
./scripts/submodules/update-submodules.sh --dry-run
```

### Add Submodule

```bash
# Add submodule at default path
./scripts/submodules/add-submodule.sh https://github.com/user/repo.git

# Add submodule at custom path
./scripts/submodules/add-submodule.sh https://github.com/user/repo.git lib/mylib

# Add submodule on specific branch
./scripts/submodules/add-submodule.sh https://github.com/user/repo.git --branch develop
```

### Remove Submodule

```bash
# Remove a submodule
./scripts/submodules/remove-submodule.sh lib/mylib

# Dry-run mode
./scripts/submodules/remove-submodule.sh lib/mylib --dry-run
```

### Execute Commands

```bash
# Check status in all submodules
./scripts/submodules/foreach-submodule.sh "git status"

# Pull latest changes in all submodules
./scripts/submodules/foreach-submodule.sh "git pull"

# Show current branch in all submodules
./scripts/submodules/foreach-submodule.sh "git branch --show-current"

# Recursive execution
./scripts/submodules/foreach-submodule.sh "git status" --recursive
```

## Use Cases

### Dependency Management
- Manage external libraries as submodules
- Pin to specific versions
- Update all dependencies at once

### Multi-Repository Projects
- Combine multiple repositories
- Maintain separate version control
- Coordinate updates across repos

### Shared Components
- Share code between projects
- Maintain single source of truth
- Track changes independently

### Vendor Dependencies
- Include third-party code
- Track upstream changes
- Easy updates and rollbacks

## Comparison: Submodule vs Subtree

See [Submodule vs Subtree Comparison](../comparisons/submodule-vs-subtree.md) for detailed comparison.

### When to Use Submodules

**Use submodules when**:
- You need to track external repositories
- Dependencies have their own release cycles
- You want to pin to specific versions
- Multiple projects share the same dependency

**Advantages**:
- Separate version control
- Clear dependency boundaries
- Easy to update to specific versions
- Smaller repository size

**Disadvantages**:
- More complex workflow
- Requires explicit updates
- Can be confusing for new users
- Extra commands needed

## Requirements

- Git 2.x or higher
- Submodule repositories must be accessible

## Best Practices

### Initialization
1. Always initialize submodules after cloning:
   ```bash
   git submodule update --init --recursive
   ```

### Updates
2. Update submodules regularly:
   ```bash
   ./scripts/submodules/update-submodules.sh --remote --recursive
   ```

### Commits
3. Commit submodule updates separately:
   ```bash
   git add .gitmodules lib/mylib
   git commit -m "Update mylib submodule to v2.0"
   ```

### Removal
4. Use the removal script for clean removal:
   ```bash
   ./scripts/submodules/remove-submodule.sh lib/mylib
   ```

### Foreach Operations
5. Test commands with dry-run first:
   ```bash
   ./scripts/submodules/foreach-submodule.sh "git status" --dry-run
   ```

## Common Workflows

### Adding a New Dependency

```bash
# Add submodule
./scripts/submodules/add-submodule.sh https://github.com/user/lib.git lib/mylib

# Initialize and update
git submodule update --init lib/mylib

# Commit
git commit -m "Add mylib as submodule"
```

### Updating All Dependencies

```bash
# Update to latest versions
./scripts/submodules/update-submodules.sh --remote --recursive

# Review changes
git diff

# Commit if satisfied
git commit -am "Update all submodules to latest versions"
```

### Removing a Dependency

```bash
# Remove submodule
./scripts/submodules/remove-submodule.sh lib/mylib

# Commit removal
git commit -m "Remove mylib submodule"
```

### Checking Status Across All Submodules

```bash
# Check status
./scripts/submodules/foreach-submodule.sh "git status"

# Check for uncommitted changes
./scripts/submodules/foreach-submodule.sh "git diff --quiet || echo 'Has changes'"
```

## Troubleshooting

### Submodule Not Initialized

**Problem**: Submodule directory is empty

**Solution**:
```bash
git submodule update --init --recursive
```

### Submodule Update Fails

**Problem**: Cannot update submodule

**Solution**:
```bash
# Check submodule status
git submodule status

# Try manual update
cd lib/mylib
git fetch
git checkout main
cd ../..
git add lib/mylib
```

### Detached HEAD in Submodule

**Problem**: Submodule is in detached HEAD state

**Solution**:
```bash
# This is normal for submodules
# To work on a branch:
cd lib/mylib
git checkout main
# Make changes, commit, push
cd ../..
git add lib/mylib
git commit -m "Update mylib"
```

### Cannot Remove Submodule

**Problem**: Removal script fails

**Solution**:
```bash
# Check for uncommitted changes
cd lib/mylib
git status

# Commit or stash changes
git stash

# Try removal again
cd ../..
./scripts/submodules/remove-submodule.sh lib/mylib
```

## See Also

- [Submodule vs Subtree Comparison](../comparisons/submodule-vs-subtree.md)
- [Subtree Management Guide](./subtree.md)
- [Git Submodule Documentation](https://git-scm.com/book/en/v2/Git-Tools-Submodules)

---

**Last Updated**: 2026-02-12
**Status**: Complete
**Scripts**: 4
