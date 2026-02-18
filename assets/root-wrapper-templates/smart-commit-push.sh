#!/usr/bin/env bash
#
# smart-commit-push.sh - Project-level wrapper for AI-powered commit and push
#
# This script points to the kano-git-master-skill combined workflow tool.
# It can be run from any directory within the project.

# Find project root (location of this script)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKILL_SCRIPT="$ROOT/skills/kano/kano-git-master-skill/scripts/commit-tools/commit-push/smart-commit-push-copilot.sh"

if [[ ! -f "$SKILL_SCRIPT" ]]; then
  echo "ERROR: Git Master Skill script not found at:"
  echo "  $SKILL_SCRIPT"
  echo "Ensure the kano-git-master-skill submodule is initialized."
  exit 1
fi

# Run the actual script (force repo root to project root)
export KANO_GIT_MASTER_ROOT="$ROOT"
exec bash "$SKILL_SCRIPT" "$@"
