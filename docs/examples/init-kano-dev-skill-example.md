# init-kano-dev-skill.sh Usage Examples

## Overview

The `init-kano-dev-skill.sh` script orchestrates the complete workflow for initializing a new Kano skill repository for development with:
- Multi-remote configuration (SSH/HTTPS fallback)
- Main branch initialization (if remote is empty)
- Orphan tooling branch creation (named after the repository for clarity)
- Development skills as submodules

The tooling branch is automatically named `dev/<repo-name>-tooling` to clearly indicate which project these development skills are for. For example, `kano-agent-skill` would get a tooling branch named `dev/kano-agent-skill-tooling`.

This is an internal tool for kano-git-master-skill to set up other Kano skill repositories for development.

## Basic Usage

### Example 1: Initialize kano-agent-skill Repository

This example initializes the `kano-agent-skill` repository at `skills/kano` with two development skills.
The tooling branch will be automatically named `dev/kano-agent-skill-tooling`.

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skill "git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git:https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git:skills/kano-filesystem-safe-ops-skill" \
  --skill "git@github.com:dorgonman/kano-agent-backlog-skill.git:https://github.com/dorgonman/kano-agent-backlog-skill.git:skills/kano-agent-backlog-skill"
```

**What this does:**
1. Checks if remote repository is empty
2. Clones or initializes repository at `skills/kano`
3. Configures multi-remote with SSH priority, HTTPS fallback
4. Initializes main branch with README.md (if remote is empty)
5. Creates orphan branch `dev/kano-agent-skill-tooling` (derived from repo name)
6. Adds two skills as submodules on the tooling branch
7. Commits and pushes changes

### Example 2: Custom Tooling Branch Name

If you want to use a different tooling branch name:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --tooling-branch dev/my-custom-tooling \
  --skill "git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git:https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git:skills/kano-filesystem-safe-ops-skill"
```

Add an upstream remote for fork workflow:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:myuser/kano-agent-skill.git \
  --repo-https https://github.com/myuser/kano-agent-skill.git \
  --upstream-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --upstream-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skill "git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git:https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git:skills/kano-filesystem-safe-ops-skill"
```

### Example 3: With Upstream Remote

Add an upstream remote for fork workflow:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:myuser/kano-agent-skill.git \
  --repo-https https://github.com/myuser/kano-agent-skill.git \
  --upstream-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --upstream-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skill "git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git:https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git:skills/kano-filesystem-safe-ops-skill"
```

Note: Tooling branch will be `dev/kano-agent-skill-tooling` (derived from repo name).

### Example 4: Dry Run Mode

Preview what would be done without making changes:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --dry-run
```

### Example 4: Dry Run Mode

Preview what would be done without making changes:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --dry-run
```

This will show you the derived tooling branch name and all steps that would be executed.

### Example 5: Skip Certain Steps

Skip main branch initialization (if you want to handle it manually):

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skip-main-init
```

Skip tooling branch creation:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skip-tooling
```

Skip skill addition:

```bash
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir skills/kano \
  --skip-skills
```

## Workflow Steps

The script executes the following steps in order:

1. **Check Remote Status**
   - Verifies remote repository is accessible
   - Determines if remote is empty or has content

2. **Initialize Repository**
   - Clones repository if directory doesn't exist
   - Uses existing directory if it already exists

3. **Configure Multi-Remote**
   - Configures root repo remotes using `kog-submodule.sh`
   - Sets up SSH/HTTPS fallback
   - Configures upstream remote if provided

4. **Initialize Main Branch** (unless `--skip-main-init`)
   - Creates initial README.md if remote is empty
   - Commits and pushes to main branch

5. **Create Tooling Branch** (unless `--skip-tooling`)
   - Derives branch name from repository name (e.g., `dev/kano-agent-skill-tooling`)
   - Creates orphan branch using `create-orphan-branch.sh`
   - Initializes with README.md explaining this is for development tools
   - Pushes to remote

6. **Add Skills** (unless `--skip-skills`)
   - Switches to tooling branch
   - Adds each skill as submodule using `kog-submodule.sh`
   - Commits and pushes changes

7. **Generate Summary**
   - Displays summary of completed operations
   - Provides next steps

## Tooling Branch Naming

The tooling branch is automatically named based on the repository name to clearly indicate which project these development skills are for:

- `kano-agent-skill` → `dev/kano-agent-skill-tooling`
- `kano-filesystem-safe-ops-skill` → `dev/kano-filesystem-safe-ops-skill-tooling`
- `my-custom-project` → `dev/my-custom-project-tooling`

This naming convention makes it clear that:
- These are development tools (not production code)
- They are specific to this project
- Different projects can have different skill combinations

You can override this with `--tooling-branch` if needed.

## Skill Format

Skills are specified using the format:

```
<ssh_url>:<https_url>:<path>
```

Example:
```
git@github.com:user/skill.git:https://github.com/user/skill.git:skills/skill-name
```

## Error Handling

The script will:
- Validate all required arguments
- Check remote accessibility before proceeding
- Report errors with descriptive messages
- Exit on critical errors (e.g., remote not accessible)

## Next Steps After Initialization

After successful initialization:

```bash
# Navigate to repository
cd skills/kano

# Switch to tooling branch to work with skills
git checkout dev/tooling

# Update skills
git submodule update --init --recursive

# Switch back to main branch
git checkout main
```

## Troubleshooting

### Remote Not Accessible

If you see "Cannot access remote repository":
- Check network connectivity
- Verify repository URL is correct
- Ensure you have access permissions
- Try HTTPS URL if SSH fails

### Directory Already Exists

If the directory already exists:
- The script will use the existing directory
- Ensure it's a valid Git repository
- Or remove the directory and try again

### Skill Addition Fails

If skill addition fails:
- Check skill repository URLs are correct
- Verify you have access to skill repositories
- Ensure skill paths don't conflict with existing files
