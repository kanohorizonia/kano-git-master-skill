#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

REPO_SSH="${REPO_SSH:-git@github.com:dorgonman/kano-agent-skill.git}"
REPO_HTTPS="${REPO_HTTPS:-https://github.com/dorgonman/kano-agent-skill.git}"
REPO_DIR="${REPO_DIR:-$SKILL_ROOT/skills/kano}"
TOOLING_BRANCH="${TOOLING_BRANCH:-dev/kano-agent-skill-tooling}"

SKILL_1_SSH="${SKILL_1_SSH:-git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git}"
SKILL_1_HTTPS="${SKILL_1_HTTPS:-https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git}"
SKILL_1_PATH="${SKILL_1_PATH:-kano-filesystem-safe-ops-skill}"

SKILL_2_SSH="${SKILL_2_SSH:-git@github.com:dorgonman/kano-agent-backlog-skill.git}"
SKILL_2_HTTPS="${SKILL_2_HTTPS:-https://github.com/dorgonman/kano-agent-backlog-skill.git}"
SKILL_2_PATH="${SKILL_2_PATH:-kano-agent-backlog-skill}"

"$SCRIPT_DIR/init-kano-dev-skill.sh" \
  --repo-ssh "$REPO_SSH" \
  --repo-https "$REPO_HTTPS" \
  --repo-dir "$REPO_DIR" \
  --tooling-branch "$TOOLING_BRANCH" \
  --update-tooling \
  --skill "$SKILL_1_SSH|$SKILL_1_HTTPS|$SKILL_1_PATH" \
  --skill "$SKILL_2_SSH|$SKILL_2_HTTPS|$SKILL_2_PATH"
