#!/usr/bin/env bash
#
# setup-multi-remote.sh - Configure multiple Git remotes with SSH/HTTP fallback
#
# Purpose:
#   Configure Git remotes with SSH and HTTP URLs for automatic fallback.
#   Supports both basic mode (single remote) and advanced mode (multi-remote).
#
# Usage:
#   ./setup-multi-remote.sh [options]
#
# Required Options (at least one):
#   --origin-ssh <url>      SSH URL for origin remote
#   --origin-http <url>     HTTP URL for origin remote
#
# Optional Options:
#   --upstream-ssh <url>    SSH URL for upstream remote
#   --upstream-http <url>   HTTP URL for upstream remote
#   --dir <path>            Repository directory (default: current directory)
#   --validate              Validate that SSH and HTTP URLs point to same repository
#   --dry-run               Show what would be done without making changes
#   -h, --help              Show help
#
# Remote Configuration Modes:
#
#   Basic Mode (single URL or no upstream):
#     origin -> SSH URL (or HTTP if only HTTP provided)
#
#   Advanced Mode (both SSH and HTTP URLs):
#     origin-ssh -> SSH URL for origin
#     origin-http -> HTTP URL for origin
#     upstream-ssh -> SSH URL for upstream (if provided)
#     upstream-http -> HTTP URL for upstream (if provided)
#
# Examples:
#   # Basic mode - single SSH URL
#   ./setup-multi-remote.sh --origin-ssh git@github.com:user/repo.git
#
#   # Basic mode - single HTTP URL
#   ./setup-multi-remote.sh --origin-http https://github.com/user/repo.git
#
#   # Advanced mode - origin with both SSH and HTTP
#   ./setup-multi-remote.sh \
#     --origin-ssh git@github.com:user/repo.git \
#     --origin-http https://github.com/user/repo.git
#
#   # Advanced mode - origin and upstream with validation
#   ./setup-multi-remote.sh \
#     --origin-ssh git@github.com:user/repo.git \
#     --origin-http https://github.com/user/repo.git \
#     --upstream-ssh git@github.com:original/repo.git \
#     --upstream-http https://github.com/original/repo.git \
#     --validate
#
#   # Dry run to preview configuration
#   ./setup-multi-remote.sh \
#     --origin-ssh git@github.com:user/repo.git \
#     --origin-http https://github.com/user/repo.git \
#     --dry-run
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

# Source git-helpers
if [[ -f "$LIB_DIR/git-helpers.sh" ]]; then
  source "$LIB_DIR/git-helpers.sh"
else
  echo "ERROR: Cannot find git-helpers.sh at $LIB_DIR/git-helpers.sh" >&2
  exit 1
fi

# Default configuration
ORIGIN_SSH=""
ORIGIN_HTTP=""
UPSTREAM_SSH=""
UPSTREAM_HTTP=""
REPO_DIR="."
VALIDATE=0
DRY_RUN=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

# Push with fallback from SSH to HTTP
# Usage: push_with_fallback <branch> [repo_dir]
# Arguments:
#   branch    - Branch name to push
#   repo_dir  - Repository directory (optional, defaults to current directory)
# Returns:
#   0 if push succeeds (via SSH or HTTP)
#   1 if both SSH and HTTP push fail
# Notes:
#   - Attempts SSH push first (origin-ssh remote)
#   - Falls back to HTTP push (origin-http remote) if SSH fails
#   - Reports detailed failure information if both attempts fail
#   - Works in both basic and advanced mode
push_with_fallback() {
  local branch="$1"
  local repo_dir="${2:-.}"

  # Validate branch name
  if [[ -z "$branch" ]]; then
    gith_error "push_with_fallback: branch name is required"
    return 1
  fi

  # Validate repository directory
  if [[ ! -d "$repo_dir" ]]; then
    gith_error "push_with_fallback: repository directory does not exist: $repo_dir"
    return 1
  fi

  # Check if it's a git repository
  if ! gith_is_git_repo "$repo_dir"; then
    gith_error "push_with_fallback: not a git repository: $repo_dir"
    return 1
  fi

  local ssh_error=""
  local http_error=""
  local ssh_remote=""
  local http_remote=""

  # Determine which remotes to use based on configuration
  # Check if advanced mode remotes exist
  if (cd "$repo_dir" && git remote get-url origin-ssh >/dev/null 2>&1); then
    ssh_remote="origin-ssh"
  elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
    # Basic mode - check if origin uses SSH or file protocol
    local origin_url
    origin_url=$(cd "$repo_dir" && git remote get-url origin)
    if [[ "$origin_url" =~ ^(git@|file://) ]]; then
      ssh_remote="origin"
    fi
  fi

  if (cd "$repo_dir" && git remote get-url origin-http >/dev/null 2>&1); then
    http_remote="origin-http"
  elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
    # Basic mode - check if origin uses HTTP
    local origin_url
    origin_url=$(cd "$repo_dir" && git remote get-url origin)
    if [[ "$origin_url" =~ ^https?:// ]]; then
      http_remote="origin"
    fi
  fi

  # Attempt SSH push first if SSH remote exists
  if [[ -n "$ssh_remote" ]]; then
    gith_log "INFO" "Attempting push to $ssh_remote..."
    if (cd "$repo_dir" && git push "$ssh_remote" "$branch" >/dev/null 2>&1); then
      gith_log "INFO" "Successfully pushed to $ssh_remote"
      return 0
    else
      gith_log "WARN" "SSH push failed"
      ssh_error="SSH push to $ssh_remote failed"
    fi
  fi

  # Fallback to HTTP push if HTTP remote exists
  if [[ -n "$http_remote" ]]; then
    gith_log "INFO" "Falling back to $http_remote..."
    if (cd "$repo_dir" && git push "$http_remote" "$branch" >/dev/null 2>&1); then
      gith_log "INFO" "Successfully pushed to $http_remote"
      return 0
    else
      gith_log "WARN" "HTTP push failed"
      http_error="HTTP push to $http_remote failed"
    fi
  fi

  # Both failed - report detailed error
  gith_error "Failed to push branch '$branch' to any remote"

  if [[ -n "$ssh_remote" && -n "$ssh_error" ]]; then
    gith_error "SSH push to $ssh_remote failed:"
    gith_error "$ssh_error"
  fi

  if [[ -n "$http_remote" && -n "$http_error" ]]; then
    gith_error "HTTP push to $http_remote failed:"
    gith_error "$http_error"
  fi

  if [[ -z "$ssh_remote" && -z "$http_remote" ]]; then
    gith_error "No suitable remotes found for push operation"
    gith_error "Expected remotes: origin-ssh, origin-http, or origin"
  fi

  return 1
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Configure multiple Git remotes with SSH/HTTP fallback capability.

Required Options (at least one):
  --origin-ssh <url>      SSH URL for origin remote
  --origin-http <url>     HTTP URL for origin remote

Optional Options:
  --upstream-ssh <url>    SSH URL for upstream remote
  --upstream-http <url>   HTTP URL for upstream remote
  --dir <path>            Repository directory (default: current directory)
  --validate              Validate that SSH and HTTP URLs point to same repository
  --dry-run               Show what would be done without making changes
  -h, --help              Show help

Remote Configuration Modes:

  Basic Mode (single URL or no upstream):
    origin -> SSH URL (or HTTP if only HTTP provided)

  Advanced Mode (both SSH and HTTP URLs):
    origin-ssh -> SSH URL for origin
    origin-http -> HTTP URL for origin
    upstream-ssh -> SSH URL for upstream (if provided)
    upstream-http -> HTTP URL for upstream (if provided)

Examples:
  # Basic mode - single SSH URL
  ./setup-multi-remote.sh --origin-ssh git@github.com:user/repo.git

  # Basic mode - single HTTP URL
  ./setup-multi-remote.sh --origin-http https://github.com/user/repo.git

  # Advanced mode - origin with both SSH and HTTP
  ./setup-multi-remote.sh \\
    --origin-ssh git@github.com:user/repo.git \\
    --origin-http https://github.com/user/repo.git

  # Advanced mode - origin and upstream with validation
  ./setup-multi-remote.sh \\
    --origin-ssh git@github.com:user/repo.git \\
    --origin-http https://github.com/user/repo.git \\
    --upstream-ssh git@github.com:original/repo.git \\
    --upstream-http https://github.com/original/repo.git \\
    --validate

  # Dry run to preview configuration
  ./setup-multi-remote.sh \\
    --origin-ssh git@github.com:user/repo.git \\
    --origin-http https://github.com/user/repo.git \\
    --dry-run

Notes:
  - At least one of --origin-ssh or --origin-http is required
  - Advanced mode is enabled when both SSH and HTTP URLs are provided
  - Use --validate to ensure SSH and HTTP URLs point to the same repository
  - Dry run mode shows what would be done without making changes
EOF
}

# Validate required arguments
validate_arguments() {
  # At least one origin URL is required
  if [[ -z "$ORIGIN_SSH" && -z "$ORIGIN_HTTP" ]]; then
    gith_error "At least one of --origin-ssh or --origin-http is required"
    usage
    exit 1
  fi

  # Validate URL formats (Requirement 7.3)
  if [[ -n "$ORIGIN_SSH" ]] && ! gith_validate_url "$ORIGIN_SSH"; then
    gith_error "Invalid origin SSH URL format: $ORIGIN_SSH"
    exit 1
  fi

  if [[ -n "$ORIGIN_HTTP" ]] && ! gith_validate_url "$ORIGIN_HTTP"; then
    gith_error "Invalid origin HTTP URL format: $ORIGIN_HTTP"
    exit 1
  fi

  if [[ -n "$UPSTREAM_SSH" ]] && ! gith_validate_url "$UPSTREAM_SSH"; then
    gith_error "Invalid upstream SSH URL format: $UPSTREAM_SSH"
    exit 1
  fi

  if [[ -n "$UPSTREAM_HTTP" ]] && ! gith_validate_url "$UPSTREAM_HTTP"; then
    gith_error "Invalid upstream HTTP URL format: $UPSTREAM_HTTP"
    exit 1
  fi

  # Validate repository directory
  if [[ ! -d "$REPO_DIR" ]]; then
    gith_error "Repository directory does not exist: $REPO_DIR"
    exit 1
  fi

  # Validate it's a git repository (Requirement 7.6)
  if ! gith_is_git_repo "$REPO_DIR"; then
    gith_error "Not a git repository: $REPO_DIR"
    exit 1
  fi

  return 0
}

# Determine configuration mode
# Returns: "basic" or "advanced"
get_config_mode() {
  # Advanced mode: both SSH and HTTP URLs provided for origin
  if [[ -n "$ORIGIN_SSH" && -n "$ORIGIN_HTTP" ]]; then
    echo "advanced"
  else
    echo "basic"
  fi
}

# Configure basic mode (single remote)
configure_basic_mode() {
  local remote_url=""

  # Determine which URL to use
  if [[ -n "$ORIGIN_SSH" ]]; then
    remote_url="$ORIGIN_SSH"
    gith_log "INFO" "Basic mode: Using SSH URL for origin"
  else
    remote_url="$ORIGIN_HTTP"
    gith_log "INFO" "Basic mode: Using HTTP URL for origin"
  fi

  # Check if origin remote already exists
  if (cd "$REPO_DIR" && git remote get-url origin >/dev/null 2>&1); then
    gith_log "INFO" "Remote 'origin' already exists, updating URL"
    if [[ "$DRY_RUN" == "1" ]]; then
      gith_log "INFO" "[DRY-RUN] Would update origin URL to: $remote_url"
    else
      (cd "$REPO_DIR" && git remote set-url origin "$remote_url")
      gith_log "INFO" "Updated origin URL to: $remote_url"
    fi
  else
    gith_log "INFO" "Creating remote 'origin'"
    if [[ "$DRY_RUN" == "1" ]]; then
      gith_log "INFO" "[DRY-RUN] Would create origin with URL: $remote_url"
    else
      (cd "$REPO_DIR" && git remote add origin "$remote_url")
      gith_log "INFO" "Created origin with URL: $remote_url"
    fi
  fi

  return 0
}

# Configure advanced mode (multi-remote)
configure_advanced_mode() {
  gith_log "INFO" "Advanced mode: Configuring multi-remote setup"

  # Configure origin-ssh
  if [[ -n "$ORIGIN_SSH" ]]; then
    if (cd "$REPO_DIR" && git remote get-url origin-ssh >/dev/null 2>&1); then
      gith_log "INFO" "Remote 'origin-ssh' already exists, updating URL"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would update origin-ssh URL to: $ORIGIN_SSH"
      else
        (cd "$REPO_DIR" && git remote set-url origin-ssh "$ORIGIN_SSH")
        gith_log "INFO" "Updated origin-ssh URL to: $ORIGIN_SSH"
      fi
    else
      gith_log "INFO" "Creating remote 'origin-ssh'"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would create origin-ssh with URL: $ORIGIN_SSH"
      else
        (cd "$REPO_DIR" && git remote add origin-ssh "$ORIGIN_SSH")
        gith_log "INFO" "Created origin-ssh with URL: $ORIGIN_SSH"
      fi
    fi
  fi

  # Configure origin-http
  if [[ -n "$ORIGIN_HTTP" ]]; then
    if (cd "$REPO_DIR" && git remote get-url origin-http >/dev/null 2>&1); then
      gith_log "INFO" "Remote 'origin-http' already exists, updating URL"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would update origin-http URL to: $ORIGIN_HTTP"
      else
        (cd "$REPO_DIR" && git remote set-url origin-http "$ORIGIN_HTTP")
        gith_log "INFO" "Updated origin-http URL to: $ORIGIN_HTTP"
      fi
    else
      gith_log "INFO" "Creating remote 'origin-http'"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would create origin-http with URL: $ORIGIN_HTTP"
      else
        (cd "$REPO_DIR" && git remote add origin-http "$ORIGIN_HTTP")
        gith_log "INFO" "Created origin-http with URL: $ORIGIN_HTTP"
      fi
    fi
  fi

  # Configure upstream-ssh
  if [[ -n "$UPSTREAM_SSH" ]]; then
    if (cd "$REPO_DIR" && git remote get-url upstream-ssh >/dev/null 2>&1); then
      gith_log "INFO" "Remote 'upstream-ssh' already exists, updating URL"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would update upstream-ssh URL to: $UPSTREAM_SSH"
      else
        (cd "$REPO_DIR" && git remote set-url upstream-ssh "$UPSTREAM_SSH")
        gith_log "INFO" "Updated upstream-ssh URL to: $UPSTREAM_SSH"
      fi
    else
      gith_log "INFO" "Creating remote 'upstream-ssh'"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would create upstream-ssh with URL: $UPSTREAM_SSH"
      else
        (cd "$REPO_DIR" && git remote add upstream-ssh "$UPSTREAM_SSH")
        gith_log "INFO" "Created upstream-ssh with URL: $UPSTREAM_SSH"
      fi
    fi
  fi

  # Configure upstream-http
  if [[ -n "$UPSTREAM_HTTP" ]]; then
    if (cd "$REPO_DIR" && git remote get-url upstream-http >/dev/null 2>&1); then
      gith_log "INFO" "Remote 'upstream-http' already exists, updating URL"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would update upstream-http URL to: $UPSTREAM_HTTP"
      else
        (cd "$REPO_DIR" && git remote set-url upstream-http "$UPSTREAM_HTTP")
        gith_log "INFO" "Updated upstream-http URL to: $UPSTREAM_HTTP"
      fi
    else
      gith_log "INFO" "Creating remote 'upstream-http'"
      if [[ "$DRY_RUN" == "1" ]]; then
        gith_log "INFO" "[DRY-RUN] Would create upstream-http with URL: $UPSTREAM_HTTP"
      else
        (cd "$REPO_DIR" && git remote add upstream-http "$UPSTREAM_HTTP")
        gith_log "INFO" "Created upstream-http with URL: $UPSTREAM_HTTP"
      fi
    fi
  fi

  return 0
}

# Validate URL pairs point to same repository
validate_url_pairs() {
  gith_log "INFO" "Validating URL pairs point to same repository"

  # Validate origin URLs if both provided
  if [[ -n "$ORIGIN_SSH" && -n "$ORIGIN_HTTP" ]]; then
    gith_log "INFO" "Validating origin SSH and HTTP URLs"
    if ! gith_validate_url_pair "$ORIGIN_SSH" "$ORIGIN_HTTP"; then
      gith_error "Origin SSH and HTTP URLs do not point to the same repository"
      gith_error "  SSH:  $ORIGIN_SSH"
      gith_error "  HTTP: $ORIGIN_HTTP"
      return 1
    fi
    gith_log "INFO" "Origin URLs validated successfully"
  fi

  # Validate upstream URLs if both provided
  if [[ -n "$UPSTREAM_SSH" && -n "$UPSTREAM_HTTP" ]]; then
    gith_log "INFO" "Validating upstream SSH and HTTP URLs"
    if ! gith_validate_url_pair "$UPSTREAM_SSH" "$UPSTREAM_HTTP"; then
      gith_error "Upstream SSH and HTTP URLs do not point to the same repository"
      gith_error "  SSH:  $UPSTREAM_SSH"
      gith_error "  HTTP: $UPSTREAM_HTTP"
      return 1
    fi
    gith_log "INFO" "Upstream URLs validated successfully"
  fi

  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Validate prerequisites
  gith_validate_prerequisites --require-git --require-git-repo --script-name "$(basename "$0")"

  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --origin-ssh)
        ORIGIN_SSH="$2"
        shift 2
        ;;
      --origin-http)
        ORIGIN_HTTP="$2"
        shift 2
        ;;
      --upstream-ssh)
        UPSTREAM_SSH="$2"
        shift 2
        ;;
      --upstream-http)
        UPSTREAM_HTTP="$2"
        shift 2
        ;;
      --dir)
        REPO_DIR="$2"
        shift 2
        ;;
      --validate)
        VALIDATE=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
    esac
  done

  # Validate arguments
  validate_arguments

  # Show configuration
  gith_log "INFO" "Repository directory: $REPO_DIR"
  if [[ -n "$ORIGIN_SSH" ]]; then
    gith_log "INFO" "Origin SSH URL: $ORIGIN_SSH"
  fi
  if [[ -n "$ORIGIN_HTTP" ]]; then
    gith_log "INFO" "Origin HTTP URL: $ORIGIN_HTTP"
  fi
  if [[ -n "$UPSTREAM_SSH" ]]; then
    gith_log "INFO" "Upstream SSH URL: $UPSTREAM_SSH"
  fi
  if [[ -n "$UPSTREAM_HTTP" ]]; then
    gith_log "INFO" "Upstream HTTP URL: $UPSTREAM_HTTP"
  fi

  # Validate URL pairs if requested
  if [[ "$VALIDATE" == "1" ]]; then
    if ! validate_url_pairs; then
      exit 1
    fi
  fi

  # Determine configuration mode
  local config_mode
  config_mode=$(get_config_mode)
  gith_log "INFO" "Configuration mode: $config_mode"

  # Configure remotes based on mode
  if [[ "$config_mode" == "basic" ]]; then
    configure_basic_mode
  else
    configure_advanced_mode
  fi

  # Show final configuration
  gith_log "INFO" "Remote configuration complete"
  if [[ "$DRY_RUN" == "0" ]]; then
    gith_log "INFO" "Current remotes:"
    (cd "$REPO_DIR" && git remote -v) | while read -r line; do
      gith_log "INFO" "  $line"
    done
  fi

  return 0
}

# Run main function
main "$@"
