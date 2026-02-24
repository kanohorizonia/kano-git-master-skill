#!/usr/bin/env bash
#
# smart-sync-upstream-force-push-copilot.sh - Copilot wrapper for smart-sync-upstream-force-push

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_MODEL="gpt-5-mini"

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

exec bash "$SCRIPT_DIR/smart-sync-upstream-force-push.sh" \
  --provider copilot \
  --model "$MODEL" \
  ${ARGS[@]+"${ARGS[@]}"}

