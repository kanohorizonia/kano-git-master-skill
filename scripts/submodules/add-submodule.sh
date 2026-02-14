#!/usr/bin/env bash
#
# add-submodule.sh - Add a submodule to the repository
#
# Usage:
#   add-submodule.sh --url <url> --path <path> [options]
#

set -euo pipefail

PREFIX=""
URL=""
BRANCH=""
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --url <url> --path <path> [options]

Add a submodule to the repository.

Required Options:
  --url <url>         Repository URL
  --path <path>       Submodule path

Options:
  --branch <branch>   Branch to track
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  $(basename "$0") --url https://github.com/user/lib.git --path lib/mylib
  $(basename "$0") --url git@github.com:org/tool.git --path vendor/tool --branch main

EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --url) URL="$2"; shift 2 ;;
    --path) PREFIX="$2"; shift 2 ;;
    --branch) BRANCH="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    *) echo "Error: Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

[[ -z "$URL" ]] && { echo "Error: URL is required" >&2; usage; exit 1; }
[[ -z "$PREFIX" ]] && { echo "Error: Path is required" >&2; usage; exit 1; }

if [[ "$DRY_RUN" -eq 1 ]]; then
  [[ -n "$BRANCH" ]] && echo "+ git submodule add -b \"$BRANCH\" \"$URL\" \"$PREFIX\"" || echo "+ git submodule add \"$URL\" \"$PREFIX\""
else
  [[ -n "$BRANCH" ]] && git submodule add -b "$BRANCH" "$URL" "$PREFIX" || git submodule add "$URL" "$PREFIX"
  echo "[INFO] Submodule added: $PREFIX"
fi
