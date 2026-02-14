# Git Worktree + Scalar Management Design

## Overview

Comprehensive design for Git worktree management with Git Scalar support for mono-repo optimization.

## Why Worktree + Orphan Branches?

### Traditional Approach (Problematic)
```bash
# Switch between branches (slow, loses context)
git checkout main
# ... work on main ...
git checkout docs  # Orphan branch
# ... work on docs ...
git checkout main  # Switch back, lose docs context
```

**Problems**:
- Switching branches is slow in large repos
- Lose IDE state, build artifacts, running processes
- Can't work on multiple branches simultaneously
- Risk of uncommitted changes

### Worktree Approach (Recommended)
```bash
# Create worktrees for different branches
git worktree add ../repo-main main
git worktree add ../repo-docs docs        # Orphan branch
git worktree add ../repo-gh-pages gh-pages  # Orphan branch

# Work simultaneously
cd ../repo-main && code .     # IDE on main
cd ../repo-docs && code .     # IDE on docs
cd ../repo-gh-pages && npm run dev  # Dev server on gh-pages
```

**Benefits**:
- ✅ Work on multiple branches simultaneously
- ✅ Each worktree has independent IDE state
- ✅ No context switching overhead
- ✅ Perfect for orphan branches (completely isolated)
- ✅ Ideal for mono-repo with multiple projects

## Git Scalar for Mono-repo

### What is Git Scalar?

Git Scalar is Microsoft's tool for optimizing Git performance in large mono-repos:
- Partial clone (blobless or treeless)
- Sparse checkout (only needed files)
- Background maintenance
- Optimized fetch/pull
- File system monitor (FSMonitor)

### When to Use Scalar?

**Use Scalar when**:
- Repo > 1GB
- > 100k files
- Slow `git status` (> 1 second)
- Mono-repo with multiple projects
- CI/CD needs faster clones

**Don't use Scalar when**:
- Small repos (< 100MB)
- Simple single-project repos
- Need full history for all files

## Proposed Architecture

### Script Organization

```
scripts/
├── worktree/                    # Worktree management
│   ├── create-worktree.sh       # Create worktree (any branch)
│   ├── create-orphan-worktree.sh # Create worktree for orphan branch
│   ├── list-worktrees.sh        # List all worktrees
│   ├── remove-worktree.sh       # Remove worktree safely
│   ├── sync-worktrees.sh        # Sync all worktrees
│   └── open-worktree.sh         # Open worktree in IDE
├── scalar/                      # Scalar management
│   ├── scalar-register.sh       # Register repo with Scalar
│   ├── scalar-unregister.sh     # Unregister repo
│   ├── scalar-status.sh         # Show Scalar status
│   └── scalar-optimize.sh       # Run Scalar optimizations
└── branch-operations/
    ├── init-branch.sh           # Universal branch init (refactored)
    └── init-orphan-branch.sh    # Orphan branch init (wrapper)
```

## Worktree Scripts Design

### 1. `create-worktree.sh` (Core)

**Purpose**: Create worktree for any branch

**Usage**:
```bash
# Create worktree for existing branch
./create-worktree.sh main

# Create worktree with custom path
./create-worktree.sh main --path ../my-main

# Create worktree and open in IDE
./create-worktree.sh main --open

# Create worktree for new branch
./create-worktree.sh feature/new --new-branch
```

**Parameters**:
- `<branch>` - Branch name (required)
- `--path <path>` - Worktree path (default: ../{repo}-{branch})
- `--new-branch` - Create new branch
- `--open` - Open in IDE after creation
- `--ide <name>` - IDE to use (code, idea, vim, etc.)

**Features**:
- Auto-generate worktree path if not specified
- Check if worktree already exists
- Support for new branch creation
- Optional IDE integration

### 2. `create-orphan-worktree.sh` (Specialized)

**Purpose**: Create worktree for orphan branch with initialization

**Usage**:
```bash
# Create orphan branch + worktree for docs
./create-orphan-worktree.sh docs

# With custom content
./create-orphan-worktree.sh docs \
  --file README.md \
  --content "# Documentation" \
  --message "docs: Initialize"

# Create and open in IDE
./create-orphan-worktree.sh gh-pages \
  --file index.html \
  --content "<h1>Site</h1>" \
  --open

# With custom path
./create-orphan-worktree.sh docs --path ~/projects/repo-docs
```

**Parameters**:
- `<branch>` - Orphan branch name (required)
- `--path <path>` - Worktree path (default: ../{repo}-{branch})
- `--file <name>` - Initial file (default: README.md)
- `--content <text>` - File content (default: "# {branch}")
- `--message <text>` - Commit message (default: "Initial commit")
- `--open` - Open in IDE
- `--push` - Push to remote after creation
- `--force-overwrite-branch` - Overwrite existing branch (DANGEROUS)

**Features**:
- Creates orphan branch if not exists
- Initializes with content
- Creates worktree automatically
- Optional push to remote
- Safety checks (branch exists, worktree exists)

### 3. `list-worktrees.sh`

**Purpose**: List all worktrees with metadata

**Usage**:
```bash
# List all worktrees
./list-worktrees.sh

# JSON output
./list-worktrees.sh --format json

# Show detailed info
./list-worktrees.sh --detailed
```

**Output Example**:
```
Worktree                        Branch      Orphan  Status      Last Commit
/home/user/repo                 main        No      Clean       abc123 feat: Add feature
/home/user/repo-docs            docs        Yes     Modified    def456 docs: Update guide
/home/user/repo-gh-pages        gh-pages    Yes     Clean       789abc chore: Update site
```

### 4. `remove-worktree.sh`

**Purpose**: Safely remove worktree

**Usage**:
```bash
# Remove worktree
./remove-worktree.sh docs

# Force remove (even with uncommitted changes)
./remove-worktree.sh docs --force

# Remove and delete branch
./remove-worktree.sh docs --delete-branch
```

**Features**:
- Check for uncommitted changes
- Warn before deletion
- Optional branch deletion
- Cleanup lock files

### 5. `sync-worktrees.sh`

**Purpose**: Sync all worktrees (fetch, pull, status)

**Usage**:
```bash
# Sync all worktrees
./sync-worktrees.sh

# Sync and show status
./sync-worktrees.sh --status

# Sync specific worktrees
./sync-worktrees.sh --worktrees "main,docs"
```

### 6. `open-worktree.sh`

**Purpose**: Open worktree in IDE

**Usage**:
```bash
# Open in default IDE (VS Code)
./open-worktree.sh docs

# Open in specific IDE
./open-worktree.sh docs --ide idea

# Open in terminal
./open-worktree.sh docs --terminal
```

## Git Scalar Scripts Design

### 1. `scalar-register.sh`

**Purpose**: Register repository with Git Scalar

**Usage**:
```bash
# Register current repo
./scalar-register.sh

# Register with specific path
./scalar-register.sh /path/to/repo

# Register with custom config
./scalar-register.sh --partial-clone --sparse-checkout
```

**Parameters**:
- `[path]` - Repository path (default: current directory)
- `--partial-clone` - Enable partial clone (blobless)
- `--sparse-checkout` - Enable sparse checkout
- `--no-fsmonitor` - Disable file system monitor

**Features**:
- Check if Scalar is installed
- Verify repo is not already registered
- Configure optimal settings
- Enable background maintenance

### 2. `scalar-unregister.sh`

**Purpose**: Unregister repository from Scalar

**Usage**:
```bash
# Unregister current repo
./scalar-unregister.sh

# Unregister specific repo
./scalar-unregister.sh /path/to/repo
```

### 3. `scalar-status.sh`

**Purpose**: Show Scalar status and configuration

**Usage**:
```bash
# Show status
./scalar-status.sh

# Show detailed config
./scalar-status.sh --detailed

# JSON output
./scalar-status.sh --format json
```

**Output Example**:
```
Scalar Status for: /home/user/repo

Registered: Yes
Partial Clone: Enabled (blobless)
Sparse Checkout: Disabled
FSMonitor: Enabled
Background Maintenance: Enabled

Performance Metrics:
  git status: 0.3s (before: 5.2s)
  git fetch: 2.1s (before: 45.3s)
  Disk usage: 1.2GB (before: 8.5GB)
```

### 4. `scalar-optimize.sh`

**Purpose**: Run Scalar optimizations

**Usage**:
```bash
# Run all optimizations
./scalar-optimize.sh

# Run specific optimization
./scalar-optimize.sh --maintenance-only

# Dry run
./scalar-optimize.sh --dry-run
```

## Branch Initialization Refactoring

### `init-branch.sh` (Refactored Core)

**Purpose**: Universal branch initialization (local or remote)

**Usage**:
```bash
# Initialize remote repository (existing functionality)
./init-branch.sh git@github.com:user/repo.git

# Create local orphan branch
./init-branch.sh --orphan docs

# Create orphan branch with worktree
./init-branch.sh --orphan docs --worktree

# Create orphan branch and push to remote
./init-branch.sh --orphan docs --push
```

**Parameters**:
- `[remote-url]` - Remote URL (for remote init)
- `--orphan <branch>` - Create orphan branch (for local init)
- `--branch <name>` - Branch name (default: main)
- `--file <name>` - Initial file (default: README.md)
- `--content <text>` - File content
- `--message <text>` - Commit message
- `--worktree` - Create worktree instead of switching
- `--worktree-path <path>` - Custom worktree path
- `--push` - Push to remote
- `--open` - Open in IDE
- `--force-overwrite-remote` - Force overwrite remote (DANGEROUS)
- `--force-overwrite-branch` - Force overwrite branch (DANGEROUS)

### `init-orphan-branch.sh` (Wrapper)

**Purpose**: Simplified wrapper for orphan branch creation

**Usage**:
```bash
# Create orphan branch with worktree (recommended)
./init-orphan-branch.sh docs

# Create orphan branch without worktree
./init-orphan-branch.sh docs --no-worktree

# Create and push
./init-orphan-branch.sh docs --push
```

**Implementation**:
```bash
#!/usr/bin/env bash
# Wrapper around init-branch.sh for orphan branches
# Default: create worktree (recommended for orphan branches)

WORKTREE=1

# Parse --no-worktree flag
for arg in "$@"; do
  if [[ "$arg" == "--no-worktree" ]]; then
    WORKTREE=0
    break
  fi
done

# Call init-branch.sh with --orphan and --worktree flags
if [[ "$WORKTREE" -eq 1 ]]; then
  exec "$(dirname "$0")/init-branch.sh" --orphan "$@" --worktree
else
  exec "$(dirname "$0")/init-branch.sh" --orphan "$@"
fi
```

## Integration Examples

### Example 1: Documentation Workflow

```bash
# Setup: Create orphan branch with worktree
./create-orphan-worktree.sh docs --open

# Work in separate directory
cd ../repo-docs
echo "# Getting Started" > getting-started.md
git add getting-started.md
git commit -m "docs: Add getting started guide"
git push origin docs

# Main branch work continues uninterrupted
cd ../repo
git checkout main
# ... work on main ...
```

### Example 2: GitHub Pages with Live Preview

```bash
# Create gh-pages worktree
./create-orphan-worktree.sh gh-pages \
  --file index.html \
  --content "<h1>Welcome</h1>"

# Start dev server in worktree
cd ../repo-gh-pages
npm install
npm run dev  # Runs on localhost:3000

# Work on main branch simultaneously
cd ../repo
code .  # IDE on main branch
```

### Example 3: Mono-repo with Scalar

```bash
# Register repo with Scalar
./scalar-register.sh --partial-clone --sparse-checkout

# Create worktrees for different projects
./create-orphan-worktree.sh project-a --open
./create-orphan-worktree.sh project-b --open
./create-orphan-worktree.sh project-c --open

# Each project in separate directory
cd ../repo-project-a && code .
cd ../repo-project-b && code .
cd ../repo-project-c && code .

# Sync all worktrees
./sync-worktrees.sh --status

# Check Scalar performance
./scalar-status.sh
```

### Example 4: CI/CD Optimization

```bash
# CI script for mono-repo
./scalar-register.sh --partial-clone

# Create worktree for specific project
./create-worktree.sh project-a --path ./build-project-a

# Build only project-a
cd ./build-project-a
npm install
npm run build

# Cleanup
./remove-worktree.sh project-a
```

## Use Cases

### 1. Documentation Management
- **Problem**: Docs mixed with code, slow to build
- **Solution**: Orphan branch + worktree for docs
- **Benefits**: Independent versioning, faster builds, clean separation

### 2. GitHub Pages
- **Problem**: Static site in same branch as code
- **Solution**: gh-pages orphan branch + worktree
- **Benefits**: Live preview, independent deployment, no code pollution

### 3. Multi-Project Mono-repo
- **Problem**: Large repo, slow operations, context switching
- **Solution**: Orphan branches per project + worktrees + Scalar
- **Benefits**: Fast operations, parallel work, isolated projects

### 4. Configuration Management
- **Problem**: Environment configs mixed with code
- **Solution**: config orphan branch + worktree
- **Benefits**: Secure configs, independent versioning, easy rollback

### 5. Asset Management
- **Problem**: Large binary files slow down repo
- **Solution**: assets orphan branch + worktree + Scalar partial clone
- **Benefits**: Fast clones, optional asset download, clean history

### 6. Localization (i18n)
- **Problem**: Translation files clutter main branch
- **Solution**: i18n orphan branch per language + worktrees
- **Benefits**: Independent translation workflow, parallel work, clean history

### 7. API Documentation
- **Problem**: Generated API docs in repo
- **Solution**: api-docs orphan branch + worktree
- **Benefits**: Auto-generated, independent versioning, no code pollution

### 8. Experimental Features
- **Problem**: Experimental code mixed with stable code
- **Solution**: experimental orphan branch + worktree
- **Benefits**: Safe experimentation, easy cleanup, no history pollution

## Technical Implementation Details

### Worktree Path Convention

```bash
# Default path pattern: ../{repo-name}-{branch-name}
# Example:
#   Repo: /home/user/my-project
#   Branch: docs
#   Worktree: /home/user/my-project-docs

generate_worktree_path() {
  local repo_name branch_name
  repo_name=$(basename "$(git rev-parse --show-toplevel)")
  branch_name="$1"
  echo "../${repo_name}-${branch_name}"
}
```

### Orphan Branch Detection

```bash
is_orphan_branch() {
  local branch="$1"
  # Check if branch has no parent commits
  local parent_count
  parent_count=$(git rev-list --parents "$branch" 2>/dev/null | tail -1 | wc -w)
  [[ "$parent_count" -eq 1 ]]
}
```

### Scalar Detection

```bash
has_scalar() {
  command -v scalar &>/dev/null
}

is_scalar_registered() {
  local repo_path="${1:-.}"
  scalar list 2>/dev/null | grep -q "$(cd "$repo_path" && pwd)"
}
```

### IDE Detection and Opening

```bash
open_in_ide() {
  local path="$1"
  local ide="${2:-auto}"
  
  case "$ide" in
    auto)
      if command -v code &>/dev/null; then
        code "$path"
      elif command -v idea &>/dev/null; then
        idea "$path"
      else
        echo "No IDE found, opening in terminal"
        cd "$path" && $SHELL
      fi
      ;;
    code|vscode)
      code "$path"
      ;;
    idea|intellij)
      idea "$path"
      ;;
    vim|nvim)
      cd "$path" && vim .
      ;;
    *)
      echo "Unknown IDE: $ide"
      return 1
      ;;
  esac
}
```

## Implementation Priority

### Phase 1: Core Worktree (Week 1)
1. ✅ `init-empty-repo.sh` (done)
2. 🔲 `create-worktree.sh` (core functionality)
3. 🔲 `list-worktrees.sh` (visibility)
4. 🔲 `remove-worktree.sh` (cleanup)

### Phase 2: Orphan Branch Integration (Week 2)
5. 🔲 Refactor `init-empty-repo.sh` → `init-branch.sh`
6. 🔲 `create-orphan-worktree.sh` (specialized)
7. 🔲 `init-orphan-branch.sh` (wrapper)

### Phase 3: Advanced Features (Week 3)
8. 🔲 `sync-worktrees.sh` (batch operations)
9. 🔲 `open-worktree.sh` (IDE integration)

### Phase 4: Scalar Integration (Week 4)
10. 🔲 `scalar-register.sh`
11. 🔲 `scalar-status.sh`
12. 🔲 `scalar-optimize.sh`
13. 🔲 `scalar-unregister.sh`

## Questions for User

1. **Worktree Path Convention**: Do you like the `../{repo}-{branch}` pattern, or prefer something else?

2. **Default Behavior**: Should `init-orphan-branch.sh` create worktree by default, or require `--worktree` flag?

3. **IDE Integration**: Which IDEs should we support? (VS Code, IntelliJ, Vim, etc.)

4. **Scalar Priority**: Should we implement Scalar scripts in Phase 4, or earlier?

5. **Naming**: Any naming preferences for the scripts?

6. **Additional Use Cases**: Any other use cases we should document?

## Next Steps

1. Review and approve design
2. Implement Phase 1 (core worktree scripts)
3. Test with real mono-repo scenarios
4. Implement Phase 2 (orphan branch integration)
5. Add Scalar support (Phase 4)
6. Update documentation and examples
