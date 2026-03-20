#!/bin/bash
set -euo pipefail

CI_MODE=false
REPO_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ci)
      CI_MODE=true
      shift
      ;;
    *)
      REPO_ROOT="$1"
      shift
      ;;
  esac
done

if [ -z "$REPO_ROOT" ]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi

if [ "$CI_MODE" = false ]; then
  bash "$REPO_ROOT/src/shell/docs/01-setup-workspace.sh"
  bash "$REPO_ROOT/src/shell/docs/02-prepare-content.sh"
  bash "$REPO_ROOT/src/shell/docs/03-build-site.sh"
  bash "$REPO_ROOT/src/shell/docs/04-build-api-docs.sh"
  bash "$REPO_ROOT/src/shell/docs/05-stage-api-docs.sh"
else
  bash "$REPO_ROOT/src/shell/docs/02-prepare-content.sh" \
    "$REPO_ROOT" \
    "$REPO_ROOT/_ws/src/raw" \
    "$REPO_ROOT/_ws/build"
  bash "$REPO_ROOT/src/shell/docs/03-build-site.sh" \
    "$REPO_ROOT" \
    "$REPO_ROOT/_ws/src/quartz" \
    "$REPO_ROOT/_ws/build" \
    "$REPO_ROOT/_ws/src/raw/src/shell/docs/config/quartz.config.template.txt"
  bash "$REPO_ROOT/src/shell/docs/04-build-api-docs.sh" \
    "$REPO_ROOT" \
    "$REPO_ROOT/_ws/src/raw" \
    "$REPO_ROOT/_ws/build"
  bash "$REPO_ROOT/src/shell/docs/05-stage-api-docs.sh" \
    "$REPO_ROOT/_ws/build"
fi

echo "Docs site ready at: $REPO_ROOT/_ws/build/public"
