#!/usr/bin/env bash
#
# smart-clone.sh - Project-level wrapper for AI-powered intelligent clone
#
# This script points to the kano-git-master-skill smart-clone tool.
# It automatically handles initialization of empty repositories.
#
# It can be run from any directory within the project.

# Find project root (location of this script)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Path to the skill's script
SKILL_SCRIPT="$ROOT/skills/kano/kano-git-master-skill/scripts/core/smart-clone.sh"
if [[ ! -f "$SKILL_SCRIPT" ]]; then
  echo "ERROR: Smart Clone script not found at:"
  echo "  $SKILL_SCRIPT"
  echo "Ensure the kano-git-master-skill submodule is initialized."
  exit 1
fi

# Run the actual script
exec bash "$SKILL_SCRIPT" "$@"
