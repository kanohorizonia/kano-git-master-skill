#!/usr/bin/env bash
set -euo pipefail

# Pause before closing when launched by double click / Git Bash file association.
# Set KANO_EXPORT_PAUSE_ON_EXIT=0 to disable.
KANO_EXPORT_PAUSE_ON_EXIT="${KANO_EXPORT_PAUSE_ON_EXIT:-1}"

pause_before_exit() {
  local exit_code="$1"

  trap - EXIT

  echo
  if [ "$exit_code" -eq 0 ]; then
    echo "Export script finished successfully."
  else
    echo "Export script failed with exit code: $exit_code"
  fi

  if [ "$KANO_EXPORT_PAUSE_ON_EXIT" != "0" ]; then
    echo
    echo "Press Enter to close..."

    if [ -r /dev/tty ]; then
      read -r _ < /dev/tty || true
    else
      read -r _ || true
    fi
  fi

  exit "$exit_code"
}

trap 'pause_before_exit $?' EXIT

# Export workspace using kog export.

PROJECT_ROOT="${1:-$(pwd)}"
OUTPUT_DIR="${2:-$PROJECT_ROOT/_exported_project}"

PROJECT_ROOT="$(cd "$PROJECT_ROOT" && pwd)"
OUTPUT_DIR="$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd)"

cd "$PROJECT_ROOT"
kog export --single --output "$OUTPUT_DIR"
