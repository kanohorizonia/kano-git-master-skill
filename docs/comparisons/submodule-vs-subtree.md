# Submodule vs Subtree - Comparison Guide

**Last Updated**: 2026-02-12

## Overview

Both Git submodules and subtrees allow you to include external repositories within your project. However, they have different philosophies, workflows, and trade-offs.

## Quick Comparison

| Feature | Submodule | Subtree |
|---------|-----------|---------|
| **Complexity** | More complex | Simpler |
| **History** | Separate | Merged |
| **Clone** | Requires `--recursive` | Automatic |
| **Updates** | Explicit commands | Explicit commands |
| **Disk Usage** | Lower (shared .git) | Higher (full copy) |
| **Contributor Friction** | Higher | Lower |
| **Upstream Changes** | Easy to push | Requires split |
| **Best For** | Active development | Vendored dependencies |

## Git Submodules

### What Are Submodules?

Submodules are references to specific commits in external repositories. The parent repository stores only a pointer (commit SHA) to the submodule, not the actual code.

### Advantages

1. **Separate History**: Submodule history is kept separate from parent repo
2. **Lower Disk Usage**: Multiple repos can share the same submodule .git directory
3. **Easy Upstream Push**: Changes can be pushed directly to submodule repo
4. **Clear Boundaries**: Explicit separation between parent and submodule code
5. **Version Pinning**: Parent repo tracks specific submodule commits

### Disadvantages

1. **Complex Workflow**: Requires understanding of submodule commands
2. **Clone Friction**: Must use `git clone --recursive` or `git submodule update --init`
3. **Update Friction**: Requires explicit `git submodule update` commands
4. **Detached HEAD**: Submodules often in detached HEAD state
5. **Contributor Confusion**: New contributors often struggle with submodules

### When to Use Submodules

- **Active Development**: You're actively developing both parent and submodule
- **Multiple Projects**: Same library used across multiple projects
- **Frequent Updates**: You need to push changes back to submodule frequently
- **Team Expertise**: Team is comfortable with Git submodules
- **Clear Ownership**: Submodule has separate maintainers/ownership

### Common Use Cases

1. **Shared Libraries**: Common code used across multiple projects
2. **Plugin Systems**: Plugins maintained in separate repositories
3. **Microservices**: Service dependencies as submodules
4. **Build Tools**: Build scripts/tools as submodules
5. **Documentation**: Separate docs repo included as submodule

## Git Subtrees

### What Are Subtrees?

Subtrees copy the entire external repository into a subdirectory of your project. The code becomes part of your repository's history.

### Advantages

1. **Simple Workflow**: Works like regular Git operations
2. **No Clone Friction**: Code is automatically available after clone
3. **No Update Friction**: No special commands needed for basic operations
4. **Normal HEAD**: No detached HEAD issues
5. **Contributor Friendly**: New contributors don't need to learn subtree commands

### Disadvantages

1. **Merged History**: External repo history merged into parent repo
2. **Higher Disk Usage**: Full copy of external repo in each clone
3. **Complex Upstream Push**: Requires `git subtree split` to push changes
4. **Larger Repository**: Parent repo size increases significantly
5. **Harder to Update**: Updating subtree requires explicit commands

### When to Use Subtrees

- **Vendored Dependencies**: You want to vendor external dependencies
- **Infrequent Updates**: External code rarely changes
- **Simple Workflow**: Team prefers simpler Git workflow
- **No Upstream Push**: You don't need to push changes back upstream
- **Self-Contained**: You want repository to be self-contained

### Common Use Cases

1. **Vendored Libraries**: Third-party libraries included in your repo
2. **Legacy Code**: Old code that rarely changes
3. **Forked Dependencies**: Forked libraries you maintain
4. **Static Assets**: Shared assets/resources
5. **Configuration**: Shared configuration files

## Detailed Comparison

### Repository Structure

**Submodule**:
```
my-project/
├── .git/
├── .gitmodules          # Submodule configuration
├── src/
└── lib/
    └── mylib/           # Submodule (pointer only)
        └── .git         # Points to .git/modules/lib/mylib
```

**Subtree**:
```
my-project/
├── .git/
├── src/
└── lib/
    └── mylib/           # Full copy of external repo
        ├── src/
        └── README.md
```

### Clone Workflow

**Submodule**:
```bash
# Clone parent repo
git clone https://github.com/user/my-project.git
cd my-project

# Initialize and update submodules (required!)
git submodule update --init --recursive

# Or clone with submodules in one step
git clone --recursive https://github.com/user/my-project.git
```

**Subtree**:
```bash
# Clone parent repo (subtree code is already there!)
git clone https://github.com/user/my-project.git
cd my-project

# No additional steps needed
```

### Add External Repository

**Submodule**:
```bash
# Add submodule
git submodule add https://github.com/user/mylib.git lib/mylib

# Commit the change
git commit -m "Add mylib submodule"
```

**Subtree**:
```bash
# Add subtree
git subtree add --prefix lib/mylib https://github.com/user/mylib.git main --squash

# Already committed
```

### Update from Upstream

**Submodule**:
```bash
# Update to latest commit on tracked branch
git submodule update --remote lib/mylib

# Commit the update
git add lib/mylib
git commit -m "Update mylib submodule"
```

**Subtree**:
```bash
# Pull updates from upstream
git subtree pull --prefix lib/mylib https://github.com/user/mylib.git main --squash
```

### Make Changes and Push Upstream

**Submodule**:
```bash
# Enter submodule directory
cd lib/mylib

# Create branch and make changes
git checkout -b feature-branch
# ... make changes ...
git commit -m "Add feature"

# Push to submodule repo
git push origin feature-branch

# Update parent repo to track new commit
cd ../..
git add lib/mylib
git commit -m "Update mylib to include feature"
```

**Subtree**:
```bash
# Make changes in subtree directory
cd lib/mylib
# ... make changes ...
cd ../..

# Commit changes in parent repo
git add lib/mylib
git commit -m "Add feature to mylib"

# Split subtree and push to upstream
git subtree push --prefix lib/mylib https://github.com/user/mylib.git feature-branch
```

### Remove External Repository

**Submodule**:
```bash
# Deinitialize submodule
git submodule deinit -f lib/mylib

# Remove from git index and working tree
git rm -f lib/mylib

# Remove from .git/modules
rm -rf .git/modules/lib/mylib

# Commit the change
git commit -m "Remove mylib submodule"
```

**Subtree**:
```bash
# Simply remove the directory
git rm -r lib/mylib

# Commit the change
git commit -m "Remove mylib subtree"
```

## Performance Comparison

### Clone Time

**Submodule**:
- Initial clone: Fast (only parent repo)
- With `--recursive`: Slower (parent + all submodules)
- Disk usage: Lower (shared .git directories)

**Subtree**:
- Initial clone: Slower (includes all subtree code)
- Disk usage: Higher (full copy of subtree)

### Update Time

**Submodule**:
- Fast (only fetches new commits)
- Requires explicit update command

**Subtree**:
- Slower (merges entire history)
- Requires explicit pull command

### Repository Size

**Submodule**:
- Parent repo: Small (only pointers)
- Total size: Sum of all repos
- Shared .git: Saves disk space

**Subtree**:
- Parent repo: Large (includes all code)
- Total size: Single large repo
- No sharing: Uses more disk space

## Migration Between Submodule and Subtree

### Submodule to Subtree

```bash
# 1. Get submodule info
SUBMODULE_PATH="lib/mylib"
SUBMODULE_URL=$(git config --file .gitmodules --get "submodule.$SUBMODULE_PATH.url")
SUBMODULE_BRANCH=$(git config --file .gitmodules --get "submodule.$SUBMODULE_PATH.branch" || echo "main")

# 2. Remove submodule
git submodule deinit -f "$SUBMODULE_PATH"
git rm -f "$SUBMODULE_PATH"
rm -rf ".git/modules/$SUBMODULE_PATH"
git commit -m "Remove $SUBMODULE_PATH submodule"

# 3. Add as subtree
git subtree add --prefix "$SUBMODULE_PATH" "$SUBMODULE_URL" "$SUBMODULE_BRANCH" --squash
```

### Subtree to Submodule

```bash
# 1. Get subtree info
SUBTREE_PATH="lib/mylib"
SUBTREE_URL="https://github.com/user/mylib.git"
SUBTREE_BRANCH="main"

# 2. Remove subtree
git rm -r "$SUBTREE_PATH"
git commit -m "Remove $SUBTREE_PATH subtree"

# 3. Add as submodule
git submodule add -b "$SUBTREE_BRANCH" "$SUBTREE_URL" "$SUBTREE_PATH"
git commit -m "Add $SUBTREE_PATH as submodule"
```

## Decision Matrix

### Choose Submodules If:

- ✅ You actively develop both parent and submodule
- ✅ You need to push changes back to submodule frequently
- ✅ Multiple projects use the same submodule
- ✅ Team is comfortable with Git submodules
- ✅ You want clear separation of concerns
- ✅ Disk space is a concern

### Choose Subtrees If:

- ✅ You want to vendor external dependencies
- ✅ External code rarely changes
- ✅ You want simpler workflow for contributors
- ✅ You rarely push changes back upstream
- ✅ You want self-contained repository
- ✅ Team is new to Git

### Avoid Both If:

- ❌ You can use package managers (npm, pip, maven, etc.)
- ❌ Dependencies are available as binary packages
- ❌ You don't need source code in your repo

## Best Practices

### Submodules

1. **Always use `--recursive`**: `git clone --recursive`
2. **Pin to specific commits**: Don't track branches directly
3. **Document workflow**: Add README with submodule instructions
4. **Use relative URLs**: For better portability
5. **Automate updates**: Use CI/CD to check for updates

### Subtrees

1. **Use `--squash`**: Keep history clean
2. **Document subtree sources**: Add README with subtree info
3. **Limit subtree size**: Don't add huge repositories
4. **Infrequent updates**: Update only when necessary
5. **Consider alternatives**: Package managers might be better

## Tools and Scripts

### Submodule Scripts

```bash
# Update all submodules to latest
./scripts/submodules/update-submodules.sh --remote --recursive

# Execute command in all submodules
./scripts/submodules/foreach-submodule.sh "git status"

# Remove submodule safely
./scripts/submodules/remove-submodule.sh lib/mylib
```

### Subtree Scripts

```bash
# Add subtree
./scripts/subtree/add-subtree.sh --prefix lib/mylib --url https://github.com/user/mylib.git

# Pull updates
./scripts/subtree/pull-subtree.sh --prefix lib/mylib

# Push changes
./scripts/subtree/push-subtree.sh --prefix lib/mylib --url https://github.com/user/mylib.git

# List all subtrees
./scripts/subtree/list-subtrees.sh
```

## Conclusion

**Submodules** are better for active development with frequent upstream pushes, while **Subtrees** are better for vendoring dependencies with infrequent updates.

**General Recommendation**: Use package managers when possible. Use submodules for active development. Use subtrees for vendoring.

---

**See Also**:
- [Submodule Guide](../guides/submodule.md)
- [Subtree Guide](../guides/subtree.md)
- [Git Submodules Official Docs](https://git-scm.com/book/en/v2/Git-Tools-Submodules)
- [Git Subtrees Official Docs](https://git-scm.com/book/en/v2/Git-Tools-Advanced-Merging#_subtree_merge)
