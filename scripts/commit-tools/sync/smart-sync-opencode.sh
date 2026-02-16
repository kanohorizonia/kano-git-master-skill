#!/usr/bin/env bash
#
# smart-sync-opencode.sh - OpenCode wrapper for smart-sync

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

exec "$SCRIPT_DIR/smart-sync.sh" \
  --provider opencode \
  --model "$MODEL" \
  ${ARGS[@]+"${ARGS[@]}"}
