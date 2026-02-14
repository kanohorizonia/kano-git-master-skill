# Root Repo Multi-Remote Configuration Examples

This document provides examples of configuring multiple remotes for a root repository using the `kog-submodule` command.

## Table of Contents

- [Basic Usage](#basic-usage)
- [Multiple Remotes](#multiple-remotes)
- [Syncing Remotes](#syncing-remotes)
- [Fork Workflow](#fork-workflow)
- [Recursive Repository Case](#recursive-repository-case)

## Basic Usage

### Configure Root Repo with Single Remote

```bash
# Configure root repo with origin remote
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --push-remote origin \
  --protocol auto
```

This creates the following `.gitmodules` configuration:

```ini
[kog-root-remote "origin"]
    kog-url-ssh = git@github.com:user/repo.git
    kog-url-https = https://github.com/user/repo.git
[kog-root-config]
    push-remote = origin
    protocol-priority = auto
```

And configures the local `.git/config`:

```ini
[remote "origin"]
    url = https://github.com/user/repo.git  # HTTPS if SSH unavailable
```

## Multiple Remotes

### Configure Root Repo with Origin and Upstream

```bash
# Configure root repo with multiple remotes
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --remote upstream \
    --ssh git@github.com:original/repo.git \
    --https https://github.com/original/repo.git \
  --push-remote origin \
  --protocol auto
```

This creates the following `.gitmodules` configuration:

```ini
[kog-root-remote "origin"]
    kog-url-ssh = git@github.com:user/repo.git
    kog-url-https = https://github.com/user/repo.git
[kog-root-remote "upstream"]
    kog-url-ssh = git@github.com:original/repo.git
    kog-url-https = https://github.com/original/repo.git
[kog-root-config]
    push-remote = origin
    protocol-priority = auto
```

And configures the local `.git/config`:

```ini
[remote "origin"]
    url = https://github.com/user/repo.git
[remote "upstream"]
    url = https://github.com/original/repo.git
```

## Syncing Remotes

### Sync Root Repo Remotes

After cloning a repository with `kog-root-remote-*` configuration, sync the remotes:

```bash
# Sync root repo remotes based on .gitmodules
kog-submodule.sh sync
```

This reads the `kog-root-remote-*` fields from `.gitmodules` and configures the remotes in `.git/config` based on SSH availability and protocol priority.

### Dry-Run Mode

Preview what would be done without making changes:

```bash
# Dry-run mode
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --dry-run
```

## Fork Workflow

### Typical Fork Workflow

```bash
# 1. Configure root repo with origin (your fork) and upstream (original repo)
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:youruser/repo.git \
    --https https://github.com/youruser/repo.git \
  --remote upstream \
    --ssh git@github.com:originaluser/repo.git \
    --https https://github.com/originaluser/repo.git \
  --push-remote origin

# 2. Fetch from all remotes
git fetch --all

# 3. Create feature branch from upstream
git checkout -b feature-branch upstream/main

# 4. Make changes and commit
git add .
git commit -m "feat: Add new feature"

# 5. Rebase onto latest upstream
git fetch upstream
git rebase upstream/main

# 6. Push to your fork
git push origin feature-branch

# 7. Create pull request on GitHub
```

## Recursive Repository Case

### Repository as Both Root Repo and Submodule

When a repository is both a root repo (with its own submodules) AND a submodule of another repo:

**In myproject's `.gitmodules`** (configures myproject's own remotes):
```ini
[kog-root-remote "origin"]
    kog-url-ssh = git@github.com:myuser/myproject.git
    kog-url-https = https://github.com/myuser/myproject.git
[kog-root-remote "upstream"]
    kog-url-ssh = git@github.com:original/myproject.git
    kog-url-https = https://github.com/original/myproject.git
[kog-root-config]
    push-remote = origin
    protocol-priority = auto
```

**In superproject's `.gitmodules`** (configures myproject as a submodule):
```ini
[submodule "myproject"]
    path = projects/myproject
    url = https://github.com/myuser/myproject.git
    kog-remote-origin-ssh = git@github.com:myuser/myproject.git
    kog-remote-origin-https = https://github.com/myuser/myproject.git
    kog-remote-upstream-ssh = git@github.com:original/myproject.git
    kog-remote-upstream-https = https://github.com/original/myproject.git
    kog-push-remote = origin
    kog-protocol-priority = auto
```

**Key Points**:
- Root repo remotes use `kog-root-remote-*` prefix in the root repo's `.gitmodules`
- Submodule remotes use `kog-remote-*` prefix in the super repo's `.gitmodules`
- Both configurations are independent and work correctly
- No filesystem traversal needed

**Commands**:
```bash
# Working inside myproject (root repo mode)
cd myproject
kog-submodule.sh sync  # Syncs myproject's own remotes

# Working from superproject (submodule mode)
cd superproject
kog-submodule.sh sync projects/myproject  # Syncs myproject as submodule
```

## Protocol Priority

### Auto (Default)

Automatically detects SSH availability and prefers SSH, falls back to HTTPS:

```bash
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --protocol auto
```

### Force SSH

Forces SSH with HTTPS fallback:

```bash
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --protocol ssh
```

### Force HTTPS

Forces HTTPS only:

```bash
kog-submodule.sh add \
  --remote origin \
    --ssh git@github.com:user/repo.git \
    --https https://github.com/user/repo.git \
  --protocol https
```

## Troubleshooting

### Remote Not Configured

If a remote is not configured after running `kog-submodule sync`:

1. Check `.gitmodules` contains the `kog-root-remote-*` fields
2. Verify both SSH and HTTPS URLs are present
3. Check the protocol priority setting
4. Run with verbose logging to see what's happening

### SSH Not Available

If SSH is not available, the system automatically falls back to HTTPS:

```bash
# Check SSH availability
ssh -T git@github.com

# If SSH fails, HTTPS will be used automatically
```

### Multiple Remotes Not Syncing

If only one remote is configured when multiple are expected:

1. Verify all remotes are defined in `.gitmodules`
2. Check for typos in remote names
3. Ensure both SSH and HTTPS URLs are provided for each remote
4. Run `kog-submodule sync` again

## See Also

- [Repository Initialization Workflow Guide](../guides/repo-initialization-workflow.md)
- [Repository Initialization Workflow Examples](./repo-initialization-workflow-examples.md)
- [Submodule Guide](../guides/submodule.md)
