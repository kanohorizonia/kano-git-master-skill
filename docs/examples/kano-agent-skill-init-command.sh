#!/usr/bin/env bash
#
# kano-agent-skill-init-command.sh - Initialize kano-agent-skill repository
#
# This script demonstrates how to initialize the kano-agent-skill repository
# with the complete workflow including multi-remote configuration, orphan branch,
# and development skills.
#
# This is run from kano-git-master-skill to set up kano-agent-skill for development.
# The tooling branch will be automatically named dev/kano-agent-skill-tooling.
#
# Usage:
#   bash docs/examples/kano-agent-skill-init-command.sh
#

set -euo pipefail

# Configuration
REPO_SSH="git@github.com:dorgonman/kano-agent-skill.git"
REPO_HTTPS="https://github.com/dorgonman/kano-agent-skill.git"
REPO_DIR="skills/kano"
# TOOLING_BRANCH will be auto-derived as "dev/kano-agent-skill-tooling"

# Skills to add
SKILL_1_SSH="git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git"
SKILL_1_HTTPS="https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git"
SKILL_1_PATH="skills/kano-filesystem-safe-ops-skill"

SKILL_2_SSH="git@github.com:dorgonman/kano-agent-backlog-skill.git"
SKILL_2_HTTPS="https://github.com/dorgonman/kano-agent-backlog-skill.git"
SKILL_2_PATH="skills/kano-agent-backlog-skill"

# Optional: Upstream remote (uncomment if needed)
# UPSTREAM_SSH="git@github.com:original/kano-agent-skill.git"
# UPSTREAM_HTTPS="https://github.com/original/kano-agent-skill.git"

echo "========================================"
echo "Initializing kano-agent-skill Repository"
echo "========================================"
echo ""
echo "This will:"
echo "  1. Check if remote repository is accessible"
echo "  2. Clone or initialize repository at: $REPO_DIR"
echo "  3. Configure multi-remote (SSH priority, HTTPS fallback)"
echo "  4. Initialize main branch (if remote is empty)"
echo "  5. Create orphan branch: dev/kano-agent-skill-tooling (auto-derived)"
echo "  6. Add development skills as submodules"
echo ""
echo "Press Ctrl+C to cancel, or Enter to continue..."
read -r

# Run the initialization workflow
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh "$REPO_SSH" \
  --repo-https "$REPO_HTTPS" \
  --repo-dir "$REPO_DIR" \
  --skill "$SKILL_1_SSH:$SKILL_1_HTTPS:$SKILL_1_PATH" \
  --skill "$SKILL_2_SSH:$SKILL_2_HTTPS:$SKILL_2_PATH"

# Optional: Add upstream remote (uncomment if needed)
# ./scripts/internal/init-kano-dev-skill.sh \
#   --repo-ssh "$REPO_SSH" \
#   --repo-https "$REPO_HTTPS" \
#   --upstream-ssh "$UPSTREAM_SSH" \
#   --upstream-https "$UPSTREAM_HTTPS" \
#   --repo-dir "$REPO_DIR" \
#   --skill "$SKILL_1_SSH:$SKILL_1_HTTPS:$SKILL_1_PATH" \
#   --skill "$SKILL_2_SSH:$SKILL_2_HTTPS:$SKILL_2_PATH"

echo ""
echo "========================================"
echo "Initialization Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  cd $REPO_DIR"
echo "  git checkout dev/kano-agent-skill-tooling  # Switch to tooling branch"
echo "  git submodule update --init --recursive  # Update skills"
echo "  git checkout main  # Switch back to main"
echo ""
