# Repository Initialization Workflow Guide

## Overview

The repository initialization workflow provides a comprehensive, automated approach to setting up Git repositories with advanced features:

- **Multi-remote configuration** with SSH/HTTPS fallback
- **Orphan branch creation** for isolated development environments
- **Enhanced submodule management** with multi-URL support
- **Intelligent error handling** with automatic recovery

## Quick Start

### Basic Initialization

Initialize a new repository with default settings:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-dir ./myproject
```

This creates:
- Main branch with README.md
- Default orphan branch `dev/gitmaster`
- Basic remote configuration

### Full-Featured Initialization

Initialize with all features enabled:

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

This creates:
- Main branch with README.md
- Multi-remote configuration (origin-ssh, origin-http, upstream-ssh, upstream-http)
- Custom orphan branch `dev/tools`
- Two submodules with multi-URL support

## Workflow Components

### 1. Remote Detection

The workflow automatically detects the remote repository status:

- **Empty**: Initializes with new content
- **Has Content**: Clones existing content
- **Not Accessible**: Reports error with diagnostics

### 2. Multi-Remote Configuration

Configure multiple remotes for flexible workflows:

**Basic Mode** (single URL):
```
origin -> git@github.com:myuser/myproject.git
```

**Advanced Mode** (SSH + HTTPS):
```
origin-ssh -> git@github.com:myuser/myproject.git
origin-http -> https://github.com/myuser/myproject.git
upstream-ssh -> git@github.com:upstream/myproject.git
upstream-http -> https://github.com/upstream/myproject.git
```

**Benefits:**
- Automatic SSH/HTTPS fallback
- Support for fork workflows
- Network resilience

### 3. Orphan Branch Creation

Create isolated branches with independent history:

```bash
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --push \
  --return
```

**Use Cases:**
- Development tooling (linters, formatters, test frameworks)
- Documentation (GitHub Pages, wikis)
- Configuration files (environment-specific settings)
- Experimental features

**Safety Features:**
- Pre-check for existing branches
- Automatic stash/restore of uncommitted changes
- Force flag with 3-second warning
- Dry-run mode

### 4. Enhanced Submodule Management

Manage submodules with multi-URL support and automatic protocol selection:

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

**Features:**
- Multiple remotes per submodule (origin, upstream, etc.)
- Automatic SSH availability detection
- Protocol fallback (SSH → HTTPS)
- Fork workflow support
- Git-compatible (preserves standard .gitmodules format)

## Workflow Execution Order

The workflow executes steps in this order:

1. **Remote Detection** - Check if remote is empty or has content
2. **Main Branch Initialization** - Initialize or clone main branch
3. **Multi-Remote Setup** - Configure multiple remotes (if HTTP URL provided)
4. **Orphan Branch Creation** - Create orphan branch (unless skipped)
5. **Submodule Addition** - Add submodules (unless skipped)
6. **Summary Report** - Display what was created/modified

## Skip Flags

Control which steps execute:

```bash
# Skip orphan branch creation
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --skip-orphan

# Skip main branch initialization (work with existing repo)
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --skip-main-init

# Skip submodule addition
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --skip-submodules
```

## Dry-Run Mode

Preview what would be done without making changes:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --dry-run
```

**Output shows:**
- Which steps would execute
- What resources would be created
- Which steps would be skipped and why

## Independent Script Usage

Each component can be used independently:

### Setup Multi-Remote

```bash
cd existing-repo
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --validate
```

### Create Orphan Branch

```bash
cd existing-repo
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --file TOOLS.md \
  --content "# Development Tools" \
  --push
```

### Manage Submodules

```bash
cd existing-repo
./scripts/submodules/kog-submodule.sh add \
  --path libs/mylib \
  --remote origin --url https://github.com/myuser/mylib.git

./scripts/submodules/kog-submodule.sh sync
./scripts/submodules/kog-submodule.sh update
```

## Error Handling

The workflow provides intelligent error handling:

### Error Categories

1. **Validation Errors** (exit code 1)
   - Invalid URLs, branch names, or paths
   - Recovery: Correct input and retry

2. **State Errors** (exit code 2)
   - Branch already exists, not in Git repo
   - Recovery: Resolve state conflict

3. **Network Errors** (exit code 3)
   - Connectivity or authentication failures
   - Recovery: Check network/credentials, automatic fallback to HTTPS

4. **Git Operation Errors** (exit code 4)
   - Git command failures
   - Recovery: Follow Git-specific recovery steps

### Automatic Recovery

The workflow includes automatic recovery mechanisms:

- **SSH Failure** → Automatic fallback to HTTPS
- **Stash Conflicts** → Detailed recovery instructions
- **Submodule Errors** → Continue with remaining submodules
- **Network Issues** → Retry with alternative protocol

## Advanced Usage

### Fork Workflow

Setup repository for fork-based development:

```bash
# Initialize with fork and upstream
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --upstream-ssh git@github.com:original/myproject.git \
  --upstream-http https://github.com/original/myproject.git \
  --repo-dir ./myproject

# Later: fetch from upstream and rebase
cd myproject
git fetch upstream
git rebase upstream/main
git push -f origin feature-branch
```

### Multi-Repository Setup

Initialize multiple repositories with same configuration:

```bash
#!/bin/bash
repos=("project1" "project2" "project3")

for repo in "${repos[@]}"; do
  ./scripts/core/init-repo-workflow.sh \
    --repo-url "git@github.com:myuser/${repo}.git" \
    --repo-http-url "https://github.com/myuser/${repo}.git" \
    --repo-dir "./${repo}" \
    --orphan-branch dev/tools
done
```

### Template-Based Initialization

Create repositories from organizational templates:

```bash
#!/bin/bash
# team-init-repo.sh - Team standard initialization

TEMPLATE_SUBMODULES=(
  "git@github.com:myorg/linter.git:tools/linter"
  "git@github.com:myorg/formatter.git:tools/formatter"
  "git@github.com:myorg/tester.git:tools/tester"
)

./scripts/core/init-repo-workflow.sh \
  --repo-url "$1" \
  --repo-http-url "$2" \
  --repo-dir "$3" \
  --orphan-branch dev/tools \
  "${TEMPLATE_SUBMODULES[@]/#/--submodule }"
```

## Gitmodules Extension Fields

The workflow uses `.gitmodules` extension fields for multi-URL configuration:

```ini
[submodule "tools/formatter"]
    path = tools/formatter
    url = https://github.com/myuser/formatter.git

    # Extension fields (preserved by Git)
    kog-remote-origin-ssh = git@github.com:myuser/formatter.git
    kog-remote-origin-https = https://github.com/myuser/formatter.git
    kog-remote-upstream-ssh = git@github.com:original/formatter.git
    kog-remote-upstream-https = https://github.com/original/formatter.git
    kog-push-remote = origin
    kog-protocol-priority = auto
```

**Benefits:**
- Single source of truth (no separate config file)
- Git-compatible (Git preserves unknown fields)
- Version-controlled and shared across machines
- Backward compatible with standard Git commands

## Best Practices

### 1. Always Use Dry-Run First

Preview changes before executing:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --dry-run
```

### 2. Provide Both SSH and HTTPS URLs

Enable automatic fallback for network resilience:

```bash
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git
```

### 3. Use Descriptive Orphan Branch Names

Make the purpose clear:

```bash
# Good
--orphan-branch dev/tools
--orphan-branch docs/wiki
--orphan-branch config/environments

# Avoid
--orphan-branch temp
--orphan-branch test
```

### 4. Validate URL Pairs

Ensure SSH and HTTP URLs point to same repository:

```bash
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --validate
```

### 5. Document Team Workflows

Create wrapper scripts for team-specific patterns:

```bash
#!/bin/bash
# team-init-repo.sh

./scripts/core/init-repo-workflow.sh \
  --repo-url "$1" \
  --repo-http-url "$2" \
  --repo-dir "$3" \
  --orphan-branch dev/tools \
  --submodule "git@github.com:myorg/linter.git:tools/linter"
```

## Troubleshooting

See [Common Pitfalls Guide](./common-pitfalls.md) and [Workflow Examples](../examples/repo-initialization-workflow-examples.md) for detailed troubleshooting.

### Quick Fixes

**Branch Already Exists**
```bash
./scripts/core/create-orphan-branch.sh --branch dev/tools-v2
# OR force overwrite (DANGEROUS)
./scripts/core/create-orphan-branch.sh --branch dev/tools --force-overwrite-branch
```

**SSH Authentication Failure**
```bash
ssh-add ~/.ssh/id_rsa
ssh -T git@github.com
# System automatically falls back to HTTPS if provided
```

**Remote Not Accessible**
```bash
ping github.com
# Try HTTPS instead
./scripts/core/init-repo-workflow.sh --repo-url https://github.com/myuser/myproject.git
```

## See Also

- [Workflow Examples](../examples/repo-initialization-workflow-examples.md) - Complete usage examples
- [Submodule Guide](./submodule.md) - Enhanced submodule management
- [Common Pitfalls](./common-pitfalls.md) - Troubleshooting guide
- [Design Document](../../.kiro/specs/repo-initialization-workflow/design.md) - Technical design
- [Requirements Document](../../.kiro/specs/repo-initialization-workflow/requirements.md) - Formal requirements
