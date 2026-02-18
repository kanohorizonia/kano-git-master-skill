#!/usr/bin/env bash
#
# smart-commit-push-opencode.sh - OpenCode wrapper for smart-commit-push

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_MODEL="auto"

MODEL="$DEFAULT_MODEL"
ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model)
      MODEL="${2:-}"
      shift 2
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

exec bash "$SCRIPT_DIR/smart-commit-push.sh" \
  --provider opencode \
  --model "$MODEL" \
  ${ARGS[@]+"${ARGS[@]}"}
