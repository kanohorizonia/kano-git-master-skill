# Git Master Skill - Usage Examples

This document provides real-world usage scenarios and examples for the Git Master Skill scripts.

## Table of Contents

- [Quick Start](#quick-start)
- [Common Workflows](#common-workflows)
- [Advanced Scenarios](#advanced-scenarios)
- [Integration Examples](#integration-examples)
- [Troubleshooting Examples](#troubleshooting-examples)

## Quick Start

### Scenario 1: Update a Single Repository

You have a repository with submodules and want to quickly update everything to the latest version.

```bash
cd /path/to/my-project
./scripts/update-repo.sh
```

**What happens:**
1. Checks for uncommitted changes (auto-stashes if found)
2. Fetches from origin
3. Rebases current branch onto remote branch
4. Updates all submodules recursively
5. Restores stashed changes

**Output:**
```
[2026-02-12 10:30:00] [INFO] Updating repository: /path/to/my-project
[2026-02-12 10:30:01] [INFO] Creating stash: auto-stash-update-repo
[2026-02-12 10:30:02] [INFO] Fetching from remote: origin
[2026-02-12 10:30:05] [INFO] Current branch: main
[2026-02-12 10:30:05] [INFO] Rebasing onto: origin/main
[2026-02-12 10:30:06] [INFO] Updating submodules...
[2026-02-12 10:30:08] [INFO] Submodule 'vendor/lib': checked out 'abc123'
[2026-02-12 10:30:09] [INFO] Popping stash: stash@{0}
[2026-02-12 10:30:10] [INFO] Update complete!
```

### Scenario 2: Clone a Fork with Upstream

You want to contribute to an open-source project by forking it.

```bash
# Clone your fork and set up upstream in one command
./scripts/clone-with-upstream.sh \
  https://github.com/yourname/awesome-project.git \
  https://github.com/original/awesome-project.git

cd awesome-project

# Later, sync with upstream
../scripts/rebase-to-upstream-latest.sh
```

**What happens:**
1. Clones your fork
2. Detects the default branch (e.g., main)
3. Checks out and pulls latest
4. Adds upstream remote
5. Fetches from upstream

## Common Workflows

### Workflow 1: Multi-Repository Workspace Management

You have a workspace with multiple repositories (root + submodules + standalone tools).

**Step 1: Discover all repositories**
```bash
cd /path/to/workspace
./scripts/discover-repos.sh --save workspace-manifest.json
```

**Output:**
```
[2026-02-12 10:30:00] [INFO] Discovering repositories...
[2026-02-12 10:30:00] [INFO] Root directory: /path/to/workspace
[2026-02-12 10:30:00] [INFO] Max depth: 3
[2026-02-12 10:30:01] [INFO] Discovered 5 repositories

root: . (main) [origin, upstream]
submodule: vendor/lib (main) [origin]
submodule: vendor/utils (develop) [origin]
standalone: tools/helper (main) [origin] *changes*
standalone: scripts/automation (main) [origin]

[2026-02-12 10:30:01] [INFO] Summary:
[2026-02-12 10:30:01] [INFO]   Total repositories: 5
[2026-02-12 10:30:01] [INFO]   Root: 1
[2026-02-12 10:30:01] [INFO]   Submodules: 2
[2026-02-12 10:30:01] [INFO]   Standalone: 2
```

**Step 2: Check status of all repositories**
```bash
./scripts/status-all-repos.sh --check-remote
```

**Output (table format):**
```
PATH                                     BRANCH          TYPE       CHANGES  UNPUSHED  STATUS
----                                     ------          ----       -------  --------  ------
.                                        main            root       0        0         up-to-date
vendor/lib                               main            submodule  0        0         up-to-date
vendor/utils                             develop         submodule  0        0         up-to-date
tools/helper                             main            standalone 3        2         ahead 2
scripts/automation                       main            standalone 0        0         up-to-date
```

**Step 3: Update all repositories**
```bash
./scripts/update-workspace-repos.sh --manifest workspace-manifest.json
```

**Output:**
```
[2026-02-12 10:35:00] [INFO] Loading repositories from manifest: workspace-manifest.json
[2026-02-12 10:35:00] [INFO] Found 5 repositories to update
[2026-02-12 10:35:00] [INFO] Remote: origin
[2026-02-12 10:35:01] [INFO] Updating: .
[2026-02-12 10:35:03] [INFO] Updated successfully: .
[2026-02-12 10:35:03] [INFO] Updating: vendor/lib
[2026-02-12 10:35:05] [INFO] Updated successfully: vendor/lib
...
[2026-02-12 10:35:20] [INFO] Summary:
[2026-02-12 10:35:20] [INFO]   Total repositories: 5
[2026-02-12 10:35:20] [INFO]   Successful: 5
[2026-02-12 10:35:20] [INFO]   Failed: 0
[2026-02-12 10:35:20] [INFO] All repositories updated successfully!
```

**Step 4: Verify updates**
```bash
./scripts/foreach-repo.sh "git status --short"
```

**Output:**
```
==> [.] (root)

==> [vendor/lib] (submodule)

==> [vendor/utils] (submodule)

==> [tools/helper] (standalone)
 M src/helper.py
?? new-feature.txt

==> [scripts/automation] (standalone)

Summary: 5 repos, 5 succeeded, 0 failed
```

### Workflow 2: Fork Contribution Workflow

You're contributing to an open-source project.

**Initial setup:**
```bash
# Clone your fork with upstream
./scripts/clone-with-upstream.sh \
  git@github.com:yourname/project.git \
  git@github.com:upstream/project.git

cd project
```

**Regular sync workflow:**
```bash
# 1. Check current status
../scripts/status-all-repos.sh

# 2. Sync with upstream
../scripts/rebase-to-upstream-latest.sh

# 3. Verify
git log --oneline -5
```

**Before creating PR:**
```bash
# Check if you're up to date with upstream
../scripts/foreach-repo.sh "git log upstream/main..HEAD --oneline"
```

### Workflow 3: Monorepo with Multiple Standalone Tools

You have a monorepo with several independent tools, each with its own Git repository.

**Discover only standalone repos:**
```bash
./scripts/discover-repos.sh --include-types standalone --format json
```

**Update only standalone repos:**
```bash
./scripts/update-workspace-repos.sh --include-types standalone
```

**Run tests in all standalone repos:**
```bash
./scripts/foreach-repo.sh "npm test" --include-types standalone --continue-on-error
```

## Advanced Scenarios

### Scenario 1: Selective Repository Updates

Update only specific types of repositories with custom exclude patterns.

```bash
# Update only submodules, excluding vendor directories
./scripts/update-workspace-repos.sh \
  --include-types submodule \
  --exclude vendor \
  --exclude node_modules
```

### Scenario 2: Dry-Run Before Actual Update

Preview what would happen without making changes.

```bash
# Dry-run to see what would be updated
./scripts/update-workspace-repos.sh --dry-run

# If everything looks good, run for real
./scripts/update-workspace-repos.sh
```

### Scenario 3: Custom Remote Names

Work with repositories that use non-standard remote names.

```bash
# Update from 'upstream' instead of 'origin'
./scripts/update-repo.sh --remote upstream

# Rebase to 'upstream/develop' instead of 'upstream/main'
./scripts/rebase-to-upstream-latest.sh --remote upstream --branch develop
```

### Scenario 4: Generate Status Reports

Create reports in different formats for different audiences.

**For terminal viewing:**
```bash
./scripts/status-all-repos.sh
```

**For CI/CD integration:**
```bash
./scripts/status-all-repos.sh --format json --output status.json
```

**For documentation:**
```bash
./scripts/status-all-repos.sh --format markdown --output STATUS.md
```

**For detailed analysis:**
```bash
./scripts/status-all-repos.sh --check-remote --format json --output detailed-status.json
```

### Scenario 5: Batch Operations Across Repos

Execute the same operation in all repositories.

**Check for uncommitted changes:**
```bash
./scripts/foreach-repo.sh "git status --porcelain"
```

**Create a branch in all repos:**
```bash
./scripts/foreach-repo.sh "git checkout -b feature/new-feature"
```

**Fetch all remotes:**
```bash
./scripts/foreach-repo.sh "git fetch --all --prune"
```

**Check for unpushed commits:**
```bash
./scripts/foreach-repo.sh "git log origin/main..HEAD --oneline"
```

## Integration Examples

### Example 1: CI/CD Pipeline Integration

**GitHub Actions workflow:**
```yaml
name: Update Dependencies

on:
  schedule:
    - cron: '0 0 * * 1'  # Weekly on Monday
  workflow_dispatch:

jobs:
  update-repos:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
        with:
          submodules: recursive
          
      - name: Discover repositories
        run: |
          ./scripts/discover-repos.sh --save repos-manifest.json
          
      - name: Check status
        run: |
          ./scripts/status-all-repos.sh --check-remote --format json --output status-before.json
          
      - name: Update all repos
        run: |
          ./scripts/update-workspace-repos.sh --manifest repos-manifest.json --continue-on-error
          
      - name: Check status after update
        run: |
          ./scripts/status-all-repos.sh --format json --output status-after.json
          
      - name: Upload artifacts
        uses: actions/upload-artifact@v3
        with:
          name: status-reports
          path: |
            status-before.json
            status-after.json
            repos-manifest.json
```

### Example 2: Pre-commit Hook

**`.git/hooks/pre-commit`:**
```bash
#!/usr/bin/env bash

# Check if any submodules have uncommitted changes
if ./scripts/foreach-repo.sh "git diff --quiet" --include-types submodule; then
  echo "All submodules are clean"
else
  echo "ERROR: Some submodules have uncommitted changes"
  echo "Please commit or stash changes in submodules first"
  exit 1
fi
```

### Example 3: Daily Status Report Script

**`daily-status.sh`:**
```bash
#!/usr/bin/env bash

DATE=$(date +%Y-%m-%d)
REPORT_DIR="reports"
mkdir -p "$REPORT_DIR"

# Generate comprehensive status report
./scripts/status-all-repos.sh \
  --check-remote \
  --format markdown \
  --output "$REPORT_DIR/status-$DATE.md"

# Generate JSON for processing
./scripts/status-all-repos.sh \
  --check-remote \
  --format json \
  --output "$REPORT_DIR/status-$DATE.json"

echo "Status report generated: $REPORT_DIR/status-$DATE.md"

# Optional: Send email or post to Slack
# mail -s "Daily Repo Status" team@example.com < "$REPORT_DIR/status-$DATE.md"
```

### Example 4: Workspace Health Check

**`health-check.sh`:**
```bash
#!/usr/bin/env bash

echo "=== Workspace Health Check ==="
echo ""

# 1. Discover all repos
echo "1. Discovering repositories..."
./scripts/discover-repos.sh --save .workspace-manifest.json
echo ""

# 2. Check for uncommitted changes
echo "2. Checking for uncommitted changes..."
./scripts/foreach-repo.sh "git status --porcelain" | grep -v "^$" || echo "All clean!"
echo ""

# 3. Check for unpushed commits
echo "3. Checking for unpushed commits..."
./scripts/foreach-repo.sh "git log origin/main..HEAD --oneline" --continue-on-error
echo ""

# 4. Generate status report
echo "4. Generating status report..."
./scripts/status-all-repos.sh --check-remote
echo ""

echo "=== Health Check Complete ==="
```

## Troubleshooting Examples

### Problem 1: Rebase Conflicts

**Scenario:** Update fails due to rebase conflicts.

```bash
# Attempt update
./scripts/update-repo.sh

# Output shows conflict
# [ERROR] Rebase failed
# [INFO] Stash preserved: stash@{0}
```

**Solution:**
```bash
# Check conflict status
git status

# Resolve conflicts in files
vim conflicted-file.txt

# Mark as resolved
git add conflicted-file.txt

# Continue rebase
git rebase --continue

# Restore stash
git stash pop stash@{0}
```

### Problem 2: Stash Recovery

**Scenario:** Script fails and leaves stash behind.

```bash
# List all stashes
git stash list

# Show what's in the stash
git stash show stash@{0} -p

# Apply stash without removing it
git stash apply stash@{0}

# After verifying, drop the stash
git stash drop stash@{0}
```

### Problem 3: Detached HEAD State

**Scenario:** Repository is in detached HEAD state.

```bash
# Check current state
git status

# Output: HEAD detached at abc123

# Option 1: Create a branch from current position
git checkout -b recovery-branch

# Option 2: Checkout to a known branch
git checkout main

# Then update normally
./scripts/update-repo.sh
```

### Problem 4: Remote Not Found

**Scenario:** Script fails because remote doesn't exist.

```bash
# Check available remotes
git remote -v

# Add missing remote
git remote add upstream https://github.com/original/repo.git

# Fetch from new remote
git fetch upstream

# Now update works
./scripts/update-repo.sh --remote upstream
```

### Problem 5: Submodule Not Initialized

**Scenario:** Submodule directories are empty.

```bash
# Initialize all submodules
git submodule update --init --recursive

# Then update
./scripts/update-repo.sh
```

## Best Practices

### 1. Always Use Dry-Run First

```bash
# Preview changes
./scripts/update-workspace-repos.sh --dry-run

# Review output, then execute
./scripts/update-workspace-repos.sh
```

### 2. Save Manifests for Repeatability

```bash
# Create manifest once
./scripts/discover-repos.sh --save .workspace-manifest.json

# Add to .gitignore if it contains sensitive paths
echo ".workspace-manifest.json" >> .gitignore

# Use manifest for consistent operations
./scripts/update-workspace-repos.sh --manifest .workspace-manifest.json
./scripts/status-all-repos.sh --manifest .workspace-manifest.json
```

### 3. Regular Status Checks

```bash
# Add to your daily routine
alias repo-status='./scripts/status-all-repos.sh --check-remote'

# Or create a cron job
# 0 9 * * * cd /path/to/workspace && ./scripts/status-all-repos.sh --check-remote --format markdown --output daily-status.md
```

### 4. Use Continue-on-Error for Batch Operations

```bash
# When updating many repos, don't stop on first failure
./scripts/update-workspace-repos.sh --continue-on-error

# Review failures at the end
# [ERROR] Failed repositories:
# [ERROR]   - tools/broken-repo
```

### 5. Combine Scripts for Powerful Workflows

```bash
# Discover → Status → Update → Verify
./scripts/discover-repos.sh --save manifest.json && \
./scripts/status-all-repos.sh --manifest manifest.json && \
./scripts/update-workspace-repos.sh --manifest manifest.json && \
./scripts/foreach-repo.sh "git status --short" --manifest manifest.json
```

## Platform-Specific Notes

### Git Bash on Windows

All scripts work on Git Bash for Windows. Some notes:

```bash
# Use forward slashes in paths
./scripts/update-repo.sh /c/Users/username/projects/myrepo

# Or use Windows-style paths (Git Bash converts automatically)
./scripts/update-repo.sh "C:\Users\username\projects\myrepo"
```

### macOS

```bash
# If you get "date: illegal option" errors, install GNU coreutils
brew install coreutils

# Use gdate instead of date
alias date=gdate
```

### Linux

Scripts work out of the box on most Linux distributions with Bash 4.0+.

## Summary

These examples cover the most common use cases for the Git Master Skill scripts. For more details on specific options and flags, use the `--help` flag on any script:

```bash
./scripts/update-repo.sh --help
./scripts/discover-repos.sh --help
./scripts/update-workspace-repos.sh --help
./scripts/foreach-repo.sh --help
./scripts/status-all-repos.sh --help
```
