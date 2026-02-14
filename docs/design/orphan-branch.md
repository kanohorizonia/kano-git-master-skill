# Orphan Branch Management Design

## Overview

Design document for orphan branch management tools in Git Master Skill.

## Use Cases

### 1. Documentation in Mono-repo
```bash
# Create orphan branch for docs
./create-orphan-branch.sh --branch docs --message "docs: Initialize documentation"

# Switch to docs branch
git checkout docs

# Work on docs independently
echo "# Documentation" > README.md
git add README.md
git commit -m "docs: Add README"
```

### 2. GitHub Pages
```bash
# Create gh-pages branch
./create-orphan-branch.sh --branch gh-pages --file index.html \
  --content "<h1>Welcome</h1>"

# Push to remote
git push -u origin gh-pages
```

### 3. Multi-Project Mono-repo
```bash
# Project A
./create-orphan-branch.sh --branch project-a --message "feat: Initialize Project A"

# Project B
./create-orphan-branch.sh --branch project-b --message "feat: Initialize Project B"

# Each project has completely independent history
```

## Proposed Scripts

### 1. `create-orphan-branch.sh` (Priority)

**Purpose**: Create orphan branch in existing repository

**Features**:
- Create orphan branch locally
- Initialize with custom content
- Optional push to remote
- Safety checks (branch already exists)
- Stash current changes before switching

**Usage**:
```bash
# Basic usage
./create-orphan-branch.sh --branch docs

# With custom content
./create-orphan-branch.sh --branch docs \
  --file README.md \
  --content "# Documentation" \
  --message "docs: Initialize"

# Create and push
./create-orphan-branch.sh --branch gh-pages \
  --file index.html \
  --content "<h1>Site</h1>" \
  --push

# Return to original branch after creation
./create-orphan-branch.sh --branch docs --return
```

**Parameters**:
- `--branch <name>` - Branch name (required)
- `--file <name>` - Initial file name (default: README.md)
- `--content <text>` - File content (default: "# {branch}")
- `--message <text>` - Commit message (default: "Initial commit")
- `--push` - Push to remote after creation
- `--return` - Return to original branch after creation
- `--force-overwrite-branch` - Overwrite existing branch (DANGEROUS)

**Safety Features**:
- Check if branch already exists (local and remote)
- Stash current changes before switching
- Restore stash if returning to original branch
- Verbose flag name for destructive operations

### 2. `switch-to-orphan.sh`

**Purpose**: Safely switch to orphan branch

**Features**:
- Auto-stash current changes
- Verify branch exists
- Show branch info (orphan status, commit count)
- Pull latest if remote exists

**Usage**:
```bash
# Switch to orphan branch
./switch-to-orphan.sh docs

# Switch and pull
./switch-to-orphan.sh docs --pull

# Switch without stash
./switch-to-orphan.sh docs --no-stash
```

### 3. `list-branches.sh`

**Purpose**: List all branches with metadata

**Features**:
- Show all branches (local and remote)
- Mark orphan branches
- Show last commit info
- Multiple output formats (table, JSON, markdown)

**Usage**:
```bash
# List all branches
./list-branches.sh

# Show only orphan branches
./list-branches.sh --orphan-only

# JSON output
./list-branches.sh --format json

# Include remote branches
./list-branches.sh --include-remote
```

**Output Example**:
```
Branch      Type     Orphan  Last Commit                    Behind/Ahead
main        local    No      abc123 feat: Add feature       origin/main
docs        local    Yes     def456 docs: Update guide      -
gh-pages    remote   Yes     789abc chore: Update site      -
```

### 4. `init-branch.sh` (Refactored from init-empty-repo.sh)

**Purpose**: Universal branch initialization (remote or orphan)

**Features**:
- Initialize remote repository (existing functionality)
- Create orphan branch in existing repo (new)
- Unified safety mechanisms
- Consistent parameter interface

**Usage**:
```bash
# Initialize remote repository (existing)
./init-branch.sh git@github.com:user/repo.git

# Create orphan branch in current repo (new)
./init-branch.sh --orphan docs

# Create orphan branch and push to remote (new)
./init-branch.sh --orphan docs --push
```

## Implementation Plan

### Phase 1: Core Functionality
1. ✅ `init-empty-repo.sh` (already done)
2. 🔲 `create-orphan-branch.sh` (priority)
3. 🔲 Refactor `init-empty-repo.sh` → `init-branch.sh` (optional)

### Phase 2: Enhanced Tools
4. 🔲 `switch-to-orphan.sh`
5. 🔲 `list-branches.sh`

### Phase 3: Advanced Features
6. 🔲 `merge-orphan-branch.sh` (special handling for orphan merges)
7. 🔲 `sync-orphan-branches.sh` (batch operations)

## Naming Considerations

### Option A: Keep `init-empty-repo.sh`, Add Orphan Tools
**Pros**: Clear separation, no breaking changes
**Cons**: Some code duplication

### Option B: Refactor to `init-branch.sh`
**Pros**: Unified interface, code reuse
**Cons**: Breaking change, more complex script

### Option C: Hybrid (Recommended)
**Pros**: Best of both worlds
**Cons**: More files to maintain

**Recommendation**: 
- Keep `init-empty-repo.sh` for remote initialization
- Add `create-orphan-branch.sh` for local orphan branches
- Share common code via `git-helpers.sh`

## Technical Details

### Orphan Branch Detection

```bash
# Check if branch is orphan
is_orphan_branch() {
  local branch="$1"
  local parent_count
  parent_count=$(git rev-list --parents "$branch" | tail -1 | wc -w)
  [[ "$parent_count" -eq 1 ]]  # Orphan has no parents
}
```

### Safe Branch Creation

```bash
# Create orphan branch safely
create_orphan_branch() {
  local branch="$1"
  
  # Check if branch exists
  if git show-ref --verify --quiet "refs/heads/$branch"; then
    echo "Branch $branch already exists"
    return 1
  fi
  
  # Stash current changes
  git stash push -m "Auto-stash before creating orphan branch $branch"
  
  # Create orphan branch
  git checkout --orphan "$branch"
  
  # Remove all files from index
  git rm -rf . 2>/dev/null || true
  
  # Add initial content
  echo "# $branch" > README.md
  git add README.md
  git commit -m "Initial commit"
}
```

### Branch Switching with Stash

```bash
# Switch to orphan branch safely
switch_to_orphan() {
  local branch="$1"
  local current_branch
  current_branch=$(git branch --show-current)
  
  # Stash if needed
  if ! git diff-index --quiet HEAD --; then
    git stash push -m "Auto-stash before switching to $branch"
  fi
  
  # Switch
  git checkout "$branch"
  
  # Pull if remote exists
  if git ls-remote --exit-code --heads origin "$branch" &>/dev/null; then
    git pull origin "$branch"
  fi
}
```

## Examples

### Example 1: Documentation Branch

```bash
# Create docs branch
./create-orphan-branch.sh --branch docs \
  --file README.md \
  --content "# Project Documentation" \
  --message "docs: Initialize documentation"

# Work on docs
cd docs/
echo "## Getting Started" >> README.md
git add README.md
git commit -m "docs: Add getting started"

# Push to remote
git push -u origin docs

# Return to main
git checkout main
```

### Example 2: GitHub Pages

```bash
# Create gh-pages branch
./create-orphan-branch.sh --branch gh-pages \
  --file index.html \
  --content "<html><body><h1>Welcome</h1></body></html>" \
  --message "chore: Initialize GitHub Pages" \
  --push

# GitHub will automatically serve from gh-pages
```

### Example 3: Multi-Project Mono-repo

```bash
# Project A
./create-orphan-branch.sh --branch project-a \
  --message "feat: Initialize Project A"

# Project B
./create-orphan-branch.sh --branch project-b \
  --message "feat: Initialize Project B"

# List all projects
./list-branches.sh --orphan-only

# Output:
# Branch      Type   Orphan  Last Commit
# project-a   local  Yes     abc123 feat: Initialize Project A
# project-b   local  Yes     def456 feat: Initialize Project B
```

## Questions for User

1. **Naming**: Do you prefer `create-orphan-branch.sh` or `init-orphan-branch.sh`?

2. **Refactoring**: Should we refactor `init-empty-repo.sh` to `init-branch.sh`, or keep them separate?

3. **Priority**: Which scripts should we implement first?
   - `create-orphan-branch.sh` (most useful)
   - `switch-to-orphan.sh` (convenience)
   - `list-branches.sh` (visibility)

4. **Features**: Any specific features you need for your use case?

5. **Return behavior**: After creating orphan branch, should script:
   - Stay on new branch (default)
   - Return to original branch (--return flag)
   - Ask user (interactive mode)

## Next Steps

1. Get feedback on design
2. Implement `create-orphan-branch.sh`
3. Add tests
4. Update documentation
5. Consider refactoring if needed
