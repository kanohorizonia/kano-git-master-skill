#!/usr/bin/env bash
#
# add-submodule.sh - Add a submodule to the repository with multi-URL support
#
# Usage:
#   add-submodule.sh --url <url> --path <path> [options]
#   add-submodule.sh --ssh-url <url> --https-url <url> --path <path> [options]
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

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

Add a submodule to the repository with optional multi-URL support.

Required Options (Basic Mode):
  --url <url>         Repository URL (SSH or HTTPS)
  --path <path>       Submodule path

Required Options (Multi-URL Mode):
  --ssh-url <url>     SSH URL for the submodule
  --https-url <url>   HTTPS URL for the submodule
  --path <path>       Submodule path

Options:
  --branch <branch>   Branch to track
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Basic mode (single URL)
  $(basename "$0") --url https://github.com/user/lib.git --path lib/mylib
  $(basename "$0") --url git@github.com:org/tool.git --path vendor/tool --branch main

  # Multi-URL mode (SSH + HTTPS with kog-* extensions)
  $(basename "$0") --ssh-url git@github.com:user/lib.git \\
                   --https-url https://github.com/user/lib.git \\
                   --path lib/mylib

EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --url) URL="$2"; shift 2 ;;
    --ssh-url) SSH_URL="$2"; shift 2 ;;
    --https-url) HTTPS_URL="$2"; shift 2 ;;
    --path) PREFIX="$2"; shift 2 ;;
    --branch) BRANCH="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    *) echo "Error: Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

# Validate prerequisites
gith_validate_prerequisites --require-git --require-git-repo --script-name "$(basename "$0")"

# Validate arguments
if [[ -z "$PREFIX" ]]; then
  echo "Error: Path is required" >&2
  usage
  exit 1
fi

# Determine mode: basic (single URL) or multi-URL (SSH + HTTPS)
MULTI_URL_MODE=0
if [[ -n "$SSH_URL" || -n "$HTTPS_URL" ]]; then
  MULTI_URL_MODE=1

  # In multi-URL mode, both SSH and HTTPS are required
  if [[ -z "$SSH_URL" || -z "$HTTPS_URL" ]]; then
    echo "Error: Both --ssh-url and --https-url are required in multi-URL mode" >&2
    usage
    exit 1
  fi

  # URL should not be specified in multi-URL mode
  if [[ -n "$URL" ]]; then
    echo "Error: Cannot specify --url with --ssh-url/--https-url" >&2
    usage
    exit 1
  fi

  # Validate URL formats (Requirement 7.3)
  if ! gith_validate_url "$SSH_URL"; then
    echo "Error: Invalid SSH URL format: $SSH_URL" >&2
    exit 1
  fi

  if ! gith_validate_url "$HTTPS_URL"; then
    echo "Error: Invalid HTTPS URL format: $HTTPS_URL" >&2
    exit 1
  fi

  # Use HTTPS URL as the default URL for git submodule add
  URL="$HTTPS_URL"
else
  # Basic mode: URL is required
  if [[ -z "$URL" ]]; then
    echo "Error: URL is required (use --url or --ssh-url/--https-url)" >&2
    usage
    exit 1
  fi

  # Validate URL format (Requirement 7.3)
  if ! gith_validate_url "$URL"; then
    echo "Error: Invalid URL format: $URL" >&2
    exit 1
  fi
fi

# Add submodule using git
  if [[ "$DRY_RUN" -eq 1 ]]; then
  if [[ -n "$BRANCH" ]]; then
    echo "+ git submodule add -b \"$BRANCH\" \"$URL\" \"$PREFIX\""
  else
    echo "+ git submodule add \"$URL\" \"$PREFIX\""
  fi

  if [[ "$MULTI_URL_MODE" -eq 1 ]]; then
    echo "+ git config -f .gitmodules \"submodule.$PREFIX.kog-url-ssh\" \"$SSH_URL\""
    echo "+ git config -f .gitmodules \"submodule.$PREFIX.kog-url-https\" \"$HTTPS_URL\""
    # Default protocol behavior is auto; no need to persist explicit field.
  fi
else
  # Add submodule
  if [[ -n "$BRANCH" ]]; then
    git submodule add -b "$BRANCH" "$URL" "$PREFIX"
  else
    git submodule add "$URL" "$PREFIX"
  fi

  # In multi-URL mode, add kog-* extension fields to .gitmodules
  if [[ "$MULTI_URL_MODE" -eq 1 ]]; then
    gith_log "INFO" "Adding kog-* extension fields to .gitmodules"
    git config -f .gitmodules "submodule.$PREFIX.kog-url-ssh" "$SSH_URL"
    git config -f .gitmodules "submodule.$PREFIX.kog-url-https" "$HTTPS_URL"

    gith_log "INFO" "Submodule added with multi-URL support: $PREFIX"
    gith_log "INFO" "  SSH URL: $SSH_URL"
    gith_log "INFO" "  HTTPS URL: $HTTPS_URL"
  else
    echo "[INFO] Submodule added: $PREFIX"
  fi
fi
