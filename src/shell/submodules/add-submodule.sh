#!/usr/bin/env bash
#
# add-submodule.sh - Compatibility wrapper for kog-submodule add
#
# Canonical command:
#   ./scripts/submodules/kog-submodule.sh add ...

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOG_SCRIPT="$SCRIPT_DIR/kog-submodule.sh"

PREFIX=""
URL=""
SSH_URL=""
HTTPS_URL=""
BRANCH=""
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --url <url> --path <path> [options]
   or: $(basename "$0") --ssh-url <url> --https-url <url> --path <path> [options]

Compatibility wrapper for:
  kog-submodule.sh add --path <path> --remote origin [--ssh ...] [--https ...]

Options:
  --url <url>         Repository URL (SSH or HTTPS)
  --ssh-url <url>     SSH URL
  --https-url <url>   HTTPS URL
  --path <path>       Submodule path
  --branch <branch>   Branch to track
  --dry-run           Show what would be done
  -h, --help          Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --url) URL="${2:-}"; shift 2 ;;
    --ssh-url) SSH_URL="${2:-}"; shift 2 ;;
    --https-url) HTTPS_URL="${2:-}"; shift 2 ;;
    --path) PREFIX="${2:-}"; shift 2 ;;
    --branch) BRANCH="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    *) echo "Error: Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ ! -f "$KOG_SCRIPT" ]]; then
  echo "Error: Missing kog-submodule script: $KOG_SCRIPT" >&2
  exit 1
fi

if [[ -z "$PREFIX" ]]; then
  echo "Error: --path is required" >&2
  usage
  exit 1
fi

if [[ -n "$URL" && ( -n "$SSH_URL" || -n "$HTTPS_URL" ) ]]; then
  echo "Error: Cannot combine --url with --ssh-url/--https-url" >&2
  usage
  exit 1
fi

if [[ -n "$URL" ]]; then
  if [[ "$URL" =~ ^https?:// ]]; then
    HTTPS_URL="$URL"
  else
    SSH_URL="$URL"
  fi
fi

if [[ -z "$SSH_URL" && -z "$HTTPS_URL" ]]; then
  echo "Error: Provide --url or at least one of --ssh-url / --https-url" >&2
  usage
  exit 1
fi

args=(add --path "$PREFIX" --remote origin)
[[ -n "$SSH_URL" ]] && args+=(--ssh "$SSH_URL")
[[ -n "$HTTPS_URL" ]] && args+=(--https "$HTTPS_URL")
[[ -n "$BRANCH" ]] && args+=(--branch "$BRANCH")
[[ "$DRY_RUN" -eq 1 ]] && args+=(--dry-run)

exec bash "$KOG_SCRIPT" "${args[@]}"
