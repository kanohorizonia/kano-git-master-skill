#!/usr/bin/env bash
#
# clone-with-upstream.sh - Clone repository with optional upstream remote
#
# Purpose:
#   Clone a repository and optionally set up an upstream remote for tracking
#   the original repository (useful for forks).
#
# Usage:
#   ./clone-with-upstream.sh <repo-url> [upstream-url] [options]
#
# Arguments:
#   repo-url          Repository URL to clone (required)
#   upstream-url      Optional upstream repository URL
#
# Options:
#   --dir <path>      Target directory (default: derived from repo name)
#   --init            Initialize with a commit if remote is empty
#   --no-checkout     Skip checkout to default branch
#   --dry-run         Show what would be done
#   -h, --help        Show help
#
# Examples:
#   # Clone without upstream
#   ./clone-with-upstream.sh https://github.com/user/repo.git
#
#   # Clone with upstream
#   ./clone-with-upstream.sh \
#     https://github.com/user/fork.git \
#     https://github.com/original/repo.git
#
#   # Clone to custom directory
#   ./clone-with-upstream.sh https://github.com/user/repo.git --dir my-project
#
# Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

REPO_URL=""
UPSTREAM_URL=""
TARGET_DIR=""
INIT_IF_EMPTY=0
NO_CHECKOUT=0
DRY_RUN=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") <repo-url> [upstream-url] [options]

Clone a repository and optionally set up an upstream remote.

Arguments:
  repo-url          Repository URL to clone (required)
  upstream-url      Optional upstream repository URL

Options:
  --dir <path>      Target directory (default: derived from repo name)
  --init            Initialize with a commit if remote is empty
  --no-checkout     Skip checkout to default branch
  --dry-run         Show what would be done
  -h, --help        Show help

Examples:
  # Clone without upstream
  ./clone-with-upstream.sh https://github.com/user/repo.git

  # Clone with upstream
  ./clone-with-upstream.sh \\
    https://github.com/user/fork.git \\
    https://github.com/original/repo.git

  # Clone to custom directory
  ./clone-with-upstream.sh https://github.com/user/repo.git --dir my-project

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
}

# Validate URL format (basic check)
validate_url() {
  local url="$1"

  # Check if URL is not empty
  if [[ -z "$url" ]]; then
    return 1
  fi

  # Check for common URL patterns (http://, https://, git@, ssh://)
  if [[ "$url" =~ ^(https?://|git@|ssh://) ]]; then
    return 0
  fi

  # Check for local path (starts with / or ./)
  if [[ "$url" =~ ^(/|\.\/) ]]; then
    return 0
  fi

  return 1
}

# Derive repository name from URL
derive_repo_name() {
  local url="$1"
  local repo_name=""

  # Extract the last part of the URL
  repo_name="$(basename "$url")"

  # Remove .git suffix if present
  repo_name="${repo_name%.git}"

  echo "$repo_name"
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --dir)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --dir requires an argument"
          usage
          exit 1
        fi
        TARGET_DIR="$2"
        shift 2
        ;;
      --no-checkout)
        NO_CHECKOUT=1
        shift
        ;;
      --init)
        INIT_IF_EMPTY=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        export DRY_RUN
        shift
        ;;
      -*)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        # First positional argument is repo URL
        if [[ -z "$REPO_URL" ]]; then
          REPO_URL="$1"
        # Second positional argument is upstream URL
        elif [[ -z "$UPSTREAM_URL" ]]; then
          UPSTREAM_URL="$1"
        else
          gith_error "Too many positional arguments"
          usage
          exit 1
        fi
        shift
        ;;
    esac
  done

  # Validate required arguments
  if [[ -z "$REPO_URL" ]]; then
    gith_error "Repository URL is required"
    usage
    exit 1
  fi

  # Validate URLs
  if ! validate_url "$REPO_URL"; then
    gith_error "Invalid repository URL: $REPO_URL"
    exit 1
  fi

  if [[ -n "$UPSTREAM_URL" ]] && ! validate_url "$UPSTREAM_URL"; then
    gith_error "Invalid upstream URL: $UPSTREAM_URL"
    exit 1
  fi

  # Derive target directory if not specified
  if [[ -z "$TARGET_DIR" ]]; then
    TARGET_DIR="$(derive_repo_name "$REPO_URL")"
    gith_log "INFO" "Target directory: $TARGET_DIR"
  fi

  # Check if target directory already exists
  if [[ -d "$TARGET_DIR" ]]; then
    gith_error "Target directory already exists: $TARGET_DIR"
    exit 1
  fi

  # Clone repository
  gith_log "INFO" "Checking remote repository: $REPO_URL"

  local is_empty=0
  if gith_is_remote_empty "$REPO_URL"; then
    is_empty=1
    gith_log "WARN" "Remote repository is empty"
  fi

  if [[ "$is_empty" -eq 1 ]]; then
    if [[ "$INIT_IF_EMPTY" -eq 1 ]]; then
      gith_log "INFO" "Initializing empty repository..."
      if [[ "$DRY_RUN" -eq 1 ]]; then
        gith_log "INFO" "[DRY-RUN] Would initialize: $REPO_URL"
      else
        # Call init-empty-repo.sh logic
        if ! "$SCRIPT_DIR/init-empty-repo.sh" "$REPO_URL" --dir "$TARGET_DIR" --keep-local; then
          gith_error "Failed to initialize repository"
          exit 1
        fi
        gith_log "INFO" "Initialization completed"
      fi
    else
      gith_error "Remote repository is empty. Use --init to initialize it."
      exit 1
    fi
  else
    gith_log "INFO" "Cloning repository: $REPO_URL"
    if [[ "$DRY_RUN" -eq 1 ]]; then
      gith_log "INFO" "[DRY-RUN] Would clone: $REPO_URL to $TARGET_DIR"
    else
      if ! git clone "$REPO_URL" "$TARGET_DIR" 2>&1; then
        gith_error "Failed to clone repository: $REPO_URL"
        exit 1
      fi
      gith_log "INFO" "Clone completed successfully"
    fi
  fi

  # Change to repository directory (skip in dry-run if doesn't exist)
  if [[ "$DRY_RUN" -eq 1 ]] && [[ ! -d "$TARGET_DIR" ]]; then
    gith_log "INFO" "[DRY-RUN] Skipping directory change and further setup"
    exit 0
  fi

  cd "$TARGET_DIR"

  # Detect remote's default branch
  gith_log "INFO" "Detecting default branch..."

  local default_branch
  default_branch="$(gith_get_default_branch "origin" ".")"

  if [[ -z "$default_branch" ]]; then
    gith_error "Could not detect default branch"
    gith_error "Repository may not have any branches"
    exit 1
  fi

  gith_log "INFO" "Default branch: $default_branch"

  # Checkout to default branch (unless --no-checkout)
  if [[ "$NO_CHECKOUT" -eq 0 ]]; then
    gith_log "INFO" "Checking out to: $default_branch"

    if [[ "$DRY_RUN" -eq 1 ]]; then
      gith_log "INFO" "[DRY-RUN] Would checkout: $default_branch"
    else
      if ! git checkout "$default_branch" 2>&1; then
        gith_error "Failed to checkout branch: $default_branch"
        exit 1
      fi

      # Pull latest changes
      gith_log "INFO" "Pulling latest changes..."
      if ! git pull origin "$default_branch" 2>&1; then
        gith_error "Failed to pull latest changes"
        exit 1
      fi

      gith_log "INFO" "Checkout completed successfully"
    fi
  else
    gith_log "INFO" "Skipping checkout (--no-checkout specified)"
  fi

  # Add upstream remote if provided
  if [[ -n "$UPSTREAM_URL" ]]; then
    gith_log "INFO" "Adding upstream remote: $UPSTREAM_URL"

    if [[ "$DRY_RUN" -eq 1 ]]; then
      gith_log "INFO" "[DRY-RUN] Would add upstream remote: $UPSTREAM_URL"
    else
      if ! git remote add upstream "$UPSTREAM_URL" 2>&1; then
        gith_error "Failed to add upstream remote"
        exit 1
      fi

      # Fetch upstream
      gith_log "INFO" "Fetching from upstream..."
      if ! git fetch upstream 2>&1; then
        gith_error "Failed to fetch from upstream"
        exit 1
      fi

      gith_log "INFO" "Upstream remote added successfully"
    fi
  fi

  # Display summary
  gith_log "INFO" "Repository setup complete!"
  gith_log "INFO" ""
  gith_log "INFO" "Summary:"
  gith_log "INFO" "  Directory: $TARGET_DIR"
  gith_log "INFO" "  Branch: $default_branch"

  if [[ "$DRY_RUN" -eq 0 ]]; then
    gith_log "INFO" "  Remotes:"
    git remote -v | while IFS= read -r line; do
      gith_log "INFO" "    $line"
    done
  fi

  exit 0
}

# Run main function
main "$@"
