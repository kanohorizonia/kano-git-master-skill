#!/usr/bin/env bash
#
# smart-status.sh - Project-level wrapper for repository status
#
# This script points to the kano-git-master-skill status tool.
# It can be run from any directory within the project.

# Find project root (location of this script)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKILL_SCRIPT="$ROOT/skills/kano/kano-git-master-skill/scripts/workspace/status-all-repos.sh"

if [[ ! -f "$SKILL_SCRIPT" ]]; then
  echo "ERROR: Git Master Skill script not found at:"
  echo "  $SKILL_SCRIPT"
  echo "Ensure the kano-git-master-skill submodule is initialized."
  exit 1
fi

# Run the actual script
exec bash "$SKILL_SCRIPT" "$@"
