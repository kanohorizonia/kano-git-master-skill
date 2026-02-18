#!/usr/bin/env bash
#
# smart-submodule.sh - Canonical submodule command entrypoint
#
# Unifies naming and dispatch for submodule operations.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
  cat <<'EOF'
Usage: smart-submodule.sh <command> [args...]

Commands:
  add         Add submodule with multi-remote support (delegates to kog-submodule.sh add)
  sync        Sync remotes / branch alignment (delegates to kog-submodule.sh sync)
  update      Update submodules (delegates to update-submodules.sh)
  remove      Remove submodule safely (delegates to remove-submodule.sh)
  foreach     Run command in each submodule (delegates to foreach-submodule.sh)
  sync-urls   Sync .gitmodules URLs to initialized submodule repos (delegates to sync-urls.sh)

Examples:
  smart-submodule.sh add --path tools/lib --remote origin --https https://github.com/org/lib.git
  smart-submodule.sh sync
  smart-submodule.sh update --recursive --remote
  smart-submodule.sh remove tools/lib
  smart-submodule.sh foreach "git status --short"
  smart-submodule.sh sync-urls --dry-run
EOF
}

cmd="${1:-}"
if [[ -z "$cmd" || "$cmd" == "-h" || "$cmd" == "--help" ]]; then
  usage
  exit 0
fi
shift || true

case "$cmd" in
  add)
    exec bash "$SCRIPT_DIR/kog-submodule.sh" add "$@"
    ;;
  sync)
    exec bash "$SCRIPT_DIR/kog-submodule.sh" sync "$@"
    ;;
  update)
    exec bash "$SCRIPT_DIR/update-submodules.sh" "$@"
    ;;
  remove)
    exec bash "$SCRIPT_DIR/remove-submodule.sh" "$@"
    ;;
  foreach)
    exec bash "$SCRIPT_DIR/foreach-submodule.sh" "$@"
    ;;
  sync-urls)
    exec bash "$SCRIPT_DIR/sync-urls.sh" "$@"
    ;;
  *)
    echo "Unknown submodule command: $cmd" >&2
    usage >&2
    exit 1
    ;;
esac

