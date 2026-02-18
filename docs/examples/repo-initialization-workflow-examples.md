# Repository Initialization Workflow - Usage Examples

This document provides comprehensive usage examples for the repository initialization workflow scripts.

## Table of Contents

- [Complete Workflow Examples](#complete-workflow-examples)
- [Multi-Remote Setup Examples](#multi-remote-setup-examples)
- [Orphan Branch Examples](#orphan-branch-examples)
- [Submodule Management Examples](#submodule-management-examples)
- [Error Scenarios and Recovery](#error-scenarios-and-recovery)

## Complete Workflow Examples

### Example 1: Initialize New Repository with All Features

Initialize a new repository with multi-remote setup, orphan branch, and submodules:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --upstream-ssh git@github.com:upstream/myproject.git \
  --upstream-http https://github.com/upstream/myproject.git \
  --repo-dir ./myproject \
  --orphan-branch dev/tools \
  --submodule "git@github.com:myuser/tool1.git:tools/tool1" \
  --submodule "git@github.com:myuser/tool2.git:tools/tool2"
```

**What this does:**
1. Detects if remote is empty or has content
2. Initializes main branch (or pulls existing content)
3. Sets up multi-remote configuration (origin-ssh, origin-http, upstream-ssh, upstream-http)
4. Creates orphan branch `dev/tools`
5. Adds two submodules to the orphan branch

### Example 2: Initialize with Basic Remote (SSH Only)

Simple initialization with single SSH URL:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-dir ./myproject
```

**What this does:**
1. Creates basic remote configuration (origin only)
2. Initializes main branch with README.md
3. Creates default orphan branch `dev/gitmaster`

### Example 3: Skip Orphan Branch Creation

Initialize without creating orphan branch:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --repo-dir ./myproject \
  --skip-orphan
```

### Example 4: Clone Existing Repository and Add Orphan Branch

Work with existing repository content:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/existing-project.git \
  --repo-http-url https://github.com/myuser/existing-project.git \
  --repo-dir ./existing-project \
  --orphan-branch dev/tools
```

**What this does:**
1. Detects remote has content
2. Clones existing repository
3. Sets up multi-remote configuration
4. Creates orphan branch for development tools

### Example 5: Dry Run Mode

Preview what would be done without making changes:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --repo-dir ./myproject \
  --orphan-branch dev/tools \
  --submodule "git@github.com:myuser/tool1.git:tools/tool1" \
  --dry-run
```

## Multi-Remote Setup Examples

### Example 6: Setup Multi-Remote for Existing Repository

Add multi-remote configuration to existing repository:

```bash
cd myproject
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --upstream-ssh git@github.com:upstream/myproject.git \
  --upstream-http https://github.com/upstream/myproject.git
```

**Result:**
```
origin-ssh -> git@github.com:myuser/myproject.git
origin-http -> https://github.com/myuser/myproject.git
upstream-ssh -> git@github.com:upstream/myproject.git
upstream-http -> https://github.com/upstream/myproject.git
```

### Example 7: Basic Remote Configuration

Setup single remote:

```bash
cd myproject
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git
```

**Result:**
```
origin -> git@github.com:myuser/myproject.git
```

### Example 8: Validate URL Pairs

Ensure SSH and HTTP URLs point to same repository:

```bash
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --validate
```

## Orphan Branch Examples

### Example 9: Create Orphan Branch with Custom Content

Create orphan branch with specific file and content:

```bash
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --file TOOLS.md \
  --content "# Development Tools\n\nThis branch contains development tooling." \
  --message "chore: Initialize tools branch"
```

### Example 10: Create and Push Orphan Branch

Create orphan branch and push to remote:

```bash
./scripts/core/create-orphan-branch.sh \
  --branch dev/docs \
  --push
```

### Example 11: Create Orphan Branch and Return

Create orphan branch but return to original branch:

```bash
./scripts/core/create-orphan-branch.sh \
  --branch dev/experiments \
  --return
```

**What this does:**
1. Stashes current changes
2. Creates orphan branch
3. Returns to original branch
4. Restores stashed changes

### Example 12: Force Overwrite Existing Branch

Overwrite existing branch (DANGEROUS):

```bash
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --force-overwrite-branch
```

**Warning:** This will display a 3-second warning before proceeding.

## Submodule Management Examples

### Example 13: Add Submodule with Multi-URL Support

Add submodule with both SSH and HTTPS URLs:

```bash
./scripts/submodules/kog-submodule.sh add \
  --path tools/formatter \
  --remote origin \
    --ssh git@github.com:myuser/formatter.git \
    --https https://github.com/myuser/formatter.git \
  --remote upstream \
    --ssh git@github.com:original/formatter.git \
    --https https://github.com/original/formatter.git
```

**Result in .gitmodules:**
```ini
[submodule "tools/formatter"]
    path = tools/formatter
    url = https://github.com/myuser/formatter.git
    kog-remote-origin-ssh = git@github.com:myuser/formatter.git
    kog-remote-origin-https = https://github.com/myuser/formatter.git
    kog-remote-upstream-ssh = git@github.com:original/formatter.git
    kog-remote-upstream-https = https://github.com/original/formatter.git
    kog-push-remote = origin
    kog-protocol-priority = auto
```

### Example 14: Add Submodule with Single URL

Add submodule with HTTPS only:

```bash
./scripts/submodules/kog-submodule.sh add \
  --path tools/linter \
  --remote origin \
    --url https://github.com/myuser/linter.git
```

### Example 15: Sync Submodule URLs

Configure local Git config based on SSH availability:

```bash
./scripts/submodules/kog-submodule.sh sync
```

**What this does:**
1. Reads kog-remote-* fields from .gitmodules
2. Tests SSH availability
3. Configures .git/modules/*/config with appropriate URLs

### Example 16: Update Submodules with Fallback

Update submodules with automatic SSH/HTTPS fallback:

```bash
./scripts/submodules/kog-submodule.sh update
```

**What this does:**
1. Attempts update with SSH (priority protocol)
2. Falls back to HTTPS if SSH fails
3. Reports which protocol was used

### Example 17: Fork Workflow with Submodules

Typical fork workflow:

```bash
# Add submodule with fork and upstream
./scripts/submodules/kog-submodule.sh add \
  --path libs/mylib \
  --remote origin \
    --ssh git@github.com:myuser/mylib.git \
    --https https://github.com/myuser/mylib.git \
  --remote upstream \
    --ssh git@github.com:original/mylib.git \
    --https https://github.com/original/mylib.git

# Later: fetch from all remotes and rebase
cd libs/mylib
git fetch --all
git rebase upstream/main
git push -f origin feature-branch
```

## Error Scenarios and Recovery

### Scenario 1: Branch Already Exists

**Error:**
```
Error: Branch 'dev/tools' already exists locally
```

**Recovery:**
```bash
# Option 1: Use different branch name
./scripts/core/create-orphan-branch.sh --branch dev/tools-v2

# Option 2: Delete existing branch first
git branch -D dev/tools
git push origin --delete dev/tools

# Option 3: Force overwrite (DANGEROUS)
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --force-overwrite-branch
```

### Scenario 2: Remote Not Accessible

**Error:**
```
Error: Remote repository not accessible
Could not connect to git@github.com:myuser/myproject.git
```

**Recovery:**
```bash
# Check SSH key
ssh -T git@github.com

# Check network connectivity
ping github.com

# Try HTTPS instead
./scripts/core/init-repo-workflow.sh \
  --repo-url https://github.com/myuser/myproject.git
```

### Scenario 3: Submodule Path Conflict

**Error:**
```
Error: Path 'tools/formatter' already exists
```

**Recovery:**
```bash
# Option 1: Use different path
./scripts/submodules/kog-submodule.sh add \
  --path tools/formatter-v2 \
  --remote origin --url https://github.com/myuser/formatter.git

# Option 2: Remove existing directory
rm -rf tools/formatter

# Option 3: Remove existing submodule
git submodule deinit tools/formatter
git rm tools/formatter
```

### Scenario 4: SSH Authentication Failure

**Error:**
```
Error: SSH authentication failed for git@github.com:myuser/myproject.git
```

**Recovery:**
```bash
# System automatically falls back to HTTPS
# No action needed if HTTPS URL was provided

# To fix SSH:
# 1. Check SSH key
ssh-add -l

# 2. Add SSH key
ssh-add ~/.ssh/id_rsa

# 3. Test SSH connection
ssh -T git@github.com
```

### Scenario 5: Stash Failure

**Error:**
```
Error: Failed to stash changes
You have unstaged changes that cannot be stashed
```

**Recovery:**
```bash
# Option 1: Commit changes
git add .
git commit -m "WIP: Save work before creating orphan branch"

# Option 2: Discard changes (DANGEROUS)
git reset --hard

# Option 3: Manually stash
git stash push -m "Manual stash before orphan branch"
```

### Scenario 6: Invalid Branch Name

**Error:**
```
Error: Invalid branch name 'dev/my branch'
Branch names cannot contain spaces
```

**Recovery:**
```bash
# Use valid branch name (no spaces, no special characters)
./scripts/core/create-orphan-branch.sh --branch dev/my-branch
```

### Scenario 7: Not in Git Repository

**Error:**
```
Error: Not in a Git repository
Current directory is not a Git repository
```

**Recovery:**
```bash
# Option 1: Initialize Git repository first
git init

# Option 2: Change to Git repository directory
cd /path/to/git/repo

# Option 3: Use --dir option
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --dir /path/to/git/repo
```

### Scenario 8: URL Pair Mismatch

**Error:**
```
Error: SSH and HTTP URLs point to different repositories
SSH: git@github.com:myuser/project1.git
HTTP: https://github.com/myuser/project2.git
```

**Recovery:**
```bash
# Correct the URLs to point to same repository
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/project1.git \
  --origin-http https://github.com/myuser/project1.git
```

## Advanced Usage Patterns

### Pattern 1: Multi-Repository Setup

Initialize multiple repositories with same configuration:

```bash
#!/bin/bash
repos=(
  "project1"
  "project2"
  "project3"
)

for repo in "${repos[@]}"; do
  ./scripts/core/init-repo-workflow.sh \
    --repo-url "git@github.com:myuser/${repo}.git" \
    --repo-http-url "https://github.com/myuser/${repo}.git" \
    --repo-dir "./${repo}" \
    --orphan-branch dev/tools
done
```

### Pattern 2: Template-Based Initialization

Create repository from template with standard submodules:

```bash
#!/bin/bash
TEMPLATE_SUBMODULES=(
  "git@github.com:myorg/linter.git:tools/linter"
  "git@github.com:myorg/formatter.git:tools/formatter"
  "git@github.com:myorg/tester.git:tools/tester"
)

./scripts/core/init-repo-workflow.sh \
  --repo-url "git@github.com:myuser/newproject.git" \
  --repo-http-url "https://github.com/myuser/newproject.git" \
  --repo-dir ./newproject \
  --orphan-branch dev/tools \
  "${TEMPLATE_SUBMODULES[@]/#/--submodule }"
```

### Pattern 3: Incremental Setup

Setup repository in stages:

```bash
# Stage 1: Initialize main branch
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-dir ./myproject \
  --skip-orphan \
  --skip-submodules

# Stage 2: Add multi-remote configuration
cd myproject
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git

# Stage 3: Create orphan branch
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools

# Stage 4: Add submodules
git checkout dev/tools
./scripts/submodules/kog-submodule.sh add \
  --path tools/tool1 \
  --remote origin --url https://github.com/myuser/tool1.git
```

## Best Practices

### 1. Always Use Dry Run First

Preview changes before executing:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --dry-run
```

### 2. Provide Both SSH and HTTPS URLs

Enable automatic fallback:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git
```

### 3. Use Descriptive Orphan Branch Names

Make purpose clear:

```bash
# Good
--orphan-branch dev/tools
--orphan-branch docs/wiki
--orphan-branch config/environments

# Avoid
--orphan-branch temp
--orphan-branch test
```

### 4. Validate URLs Before Setup

Use --validate flag:

```bash
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --validate
```

### 5. Document Custom Workflows

Create wrapper scripts for team-specific workflows:

```bash
#!/bin/bash
# team-init-repo.sh - Team standard repository initialization

./scripts/core/init-repo-workflow.sh \
  --repo-url "$1" \
  --repo-http-url "$2" \
  --repo-dir "$3" \
  --orphan-branch dev/tools \
  --submodule "git@github.com:myorg/linter.git:tools/linter" \
  --submodule "git@github.com:myorg/formatter.git:tools/formatter"
```

## See Also

- [Quick Start Guide](../guides/quick-start.md)
- [Submodule Guide](../guides/submodule.md)
- [Troubleshooting Guide](../guides/common-pitfalls.md)
- [Design Document](../../.kiro/specs/repo-initialization-workflow/design.md)
- [Requirements Document](../../.kiro/specs/repo-initialization-workflow/requirements.md)
