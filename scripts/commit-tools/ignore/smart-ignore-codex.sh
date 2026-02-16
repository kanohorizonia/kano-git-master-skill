#!/usr/bin/env bash
#
# smart-ignore-codex.sh - Codex wrapper for smart-ignore

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_MODEL="gpt-5.3-codex"

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

exec bash "$SCRIPT_DIR/smart-ignore.sh" \
  --provider codex \
  --model "$MODEL" \
  ${ARGS[@]+"${ARGS[@]}"}
