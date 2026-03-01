#!/usr/bin/env bash
#
# kog-submodule.sh - Enhanced submodule management with multi-remote support
#
# This script provides enhanced submodule commands that support multiple remotes
# with automatic protocol selection (SSH/HTTPS fallback).
#
# Commands:
#   add     - Add submodule with multi-remote support
#   sync    - Sync submodule remotes based on kog-* fields
#   update  - Update submodules with protocol fallback
#   fetch   - Fetch from all configured remotes
#   push    - Push to configured remote with fallback
#
# Usage:
#   kog-submodule.sh add --path <path> --remote <name> --ssh <url> --https <url>
#   kog-submodule.sh sync [path]
#   kog-submodule.sh update [path]
#   kog-submodule.sh fetch [path]
#   kog-submodule.sh push [path] [branch] [--force]
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

# Script version
KOG_SUBMODULE_VERSION="0.1.0"

#------------------------------------------------------------------------------
# Usage Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") <command> [options]

Enhanced submodule management with multi-remote support.

Commands:
  add       Add submodule with multi-remote support
  sync      Sync submodule remotes based on kog-* fields
  update    Update submodules with protocol fallback
  fetch     Fetch from all configured remotes
  push      Push to configured remote with fallback

Run '$(basename "$0") <command> --help' for command-specific help.

EOF
}

usage_add() {
  cat << EOF
Usage: $(basename "$0") add [--path <path>] [options]

Add a submodule with multi-remote support, or configure root repo remotes.

Mode Selection:
  --path <path>           Submodule mode: Add submodule at path
  (no --path)             Root repo mode: Configure current repo's remotes

Remote Configuration (repeatable):
  --remote <name>         Remote name (e.g., origin, upstream)
    --ssh <url>           SSH URL for this remote
    --https <url>         HTTPS URL for this remote

Options:
  --push-remote <name>    Remote to use for push (default: origin)
  --protocol <priority>   Protocol priority: auto|ssh|https (default: auto)
  --branch <branch>       Branch to track (submodule mode only)
  --dry-run               Show what would be done
  -h, --help              Show this help

Examples:
  # Configure root repo with multiple remotes
  $(basename "$0") add \\
    --remote origin \\
      --ssh git@github.com:user/repo.git \\
      --https https://github.com/user/repo.git \\
    --remote upstream \\
      --ssh git@github.com:original/repo.git \\
      --https https://github.com/original/repo.git \\
    --push-remote origin

  # Add submodule with single remote (origin)
  $(basename "$0") add --path skills/repo \\
    --remote origin \\
      --ssh git@github.com:user/repo.git \\
      --https https://github.com/user/repo.git

  # Add submodule with multiple remotes (origin + upstream)
  $(basename "$0") add --path skills/repo \\
    --remote origin \\
      --ssh git@github.com:user/repo.git \\
      --https https://github.com/user/repo.git \\
    --remote upstream \\
      --ssh git@github.com:original/repo.git \\
      --https https://github.com/original/repo.git \\
    --push-remote origin

EOF
}

usage_sync() {
  cat << EOF
Usage: $(basename "$0") sync [path]

Sync remotes based on kog-* extension fields in .gitmodules.

Mode Selection:
  [path]      Submodule mode: Sync specific submodule
  (no path)   Root repo mode: Sync current repo's remotes (if no submodules)
              OR sync all submodules (if submodules exist)

Behavior:
  - Submodule mode: Configures .git/modules/<submodule>/config
  - Root repo mode: Configures .git/config
  - URLs selected based on SSH availability and protocol priority

Options:
  --dry-run   Show what would be done
  -h, --help  Show this help

Examples:
  # Sync root repo remotes (when no submodules exist)
  $(basename "$0") sync

  # Sync all submodules (when submodules exist)
  $(basename "$0") sync

  # Sync specific submodule
  $(basename "$0") sync skills/repo

EOF
}

usage_update() {
  cat << EOF
Usage: $(basename "$0") update [path]

Update submodules with automatic protocol fallback.
Attempts SSH first, falls back to HTTPS if SSH fails.

Options:
  [path]      Specific submodule path (default: all submodules)
  --dry-run   Show what would be done
  -h, --help  Show this help

Examples:
  # Update all submodules
  $(basename "$0") update

  # Update specific submodule
  $(basename "$0") update skills/repo

EOF
}

#------------------------------------------------------------------------------
# Helper Functions
#------------------------------------------------------------------------------

# Parse remote configuration from command line
# Expects arguments in format: --remote <name> --ssh <url> --https <url>
# Stores results in associative arrays: REMOTE_SSH_URLS, REMOTE_HTTPS_URLS
parse_remote_config() {
  local current_remote=""

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --remote)
        current_remote="$2"
        shift 2
        ;;
      --ssh)
        if [[ -z "$current_remote" ]]; then
          gith_error "Error: --ssh must follow --remote <name>"
          return 1
        fi
        REMOTE_SSH_URLS["$current_remote"]="$2"
        shift 2
        ;;
      --https)
        if [[ -z "$current_remote" ]]; then
          gith_error "Error: --https must follow --remote <name>"
          return 1
        fi
        REMOTE_HTTPS_URLS["$current_remote"]="$2"
        shift 2
        ;;
      *)
        # Unknown option, return to caller
        return 0
        ;;
    esac
  done
}

# Get kog-* field from .gitmodules
get_kog_field() {
  local submodule_path="$1"
  local field_name="$2"
  local default_value="${3:-}"

  local value
  value=$(git config -f .gitmodules "submodule.$submodule_path.$field_name" 2>/dev/null || echo "$default_value")
  echo "$value"
}

# Set kog-* field in .gitmodules
set_kog_field() {
  local submodule_path="$1"
  local field_name="$2"
  local value="$3"

  # Default behavior: do not persist explicit "auto" for protocol priority.
  if [[ "$field_name" == "kog-protocol-priority" && "$value" == "auto" ]]; then
    git config -f .gitmodules --unset-all "submodule.$submodule_path.$field_name" 2>/dev/null || true
    return 0
  fi

  git config -f .gitmodules "submodule.$submodule_path.$field_name" "$value"
}

# Get all remote names for a submodule from kog-* fields
get_submodule_remotes() {
  local submodule_path="$1"

  # Get all kog-remote-* fields and extract the remote name
  # Handles both kog-remote-<name> and kog-remote-<name>-<suffix>
  git config -f .gitmodules --get-regexp "submodule\.$submodule_path\.kog-remote-.*" 2>/dev/null | \
    sed -E "s|submodule\.$submodule_path\.kog-remote-([^.-]+).*|\1|" | \
    sort -u || true
}

# Get all remote names for root repo from kog-root-remote-* fields
get_root_repo_remotes() {
  # Get all kog-root-remote sections and extract the remote name
  git config -f .gitmodules --get-regexp "kog-root-remote\..*" 2>/dev/null | \
    sed -E 's/kog-root-remote\.([^.]+)\..*/\1/' | \
    sort -u || true
}

# Get kog-root-remote-* field from .gitmodules
get_root_kog_field() {
  local remote_name="$1"
  local field_name="$2"
  local default_value="${3:-}"

  local value
  value=$(git config -f .gitmodules "kog-root-remote.$remote_name.$field_name" 2>/dev/null || echo "$default_value")
  echo "$value"
}

# Set kog-root-remote-* field in .gitmodules
set_root_kog_field() {
  local remote_name="$1"
  local field_name="$2"
  local value="$3"

  git config -f .gitmodules "kog-root-remote.$remote_name.$field_name" "$value"
}

# Get root repo configuration field
get_root_config_field() {
  local field_name="$1"
  local default_value="${2:-}"

  local value
  # Use kog-root-config section for configuration fields
  value=$(git config -f .gitmodules "kog-root-config.$field_name" 2>/dev/null || echo "$default_value")
  echo "$value"
}

# Set root repo configuration field
set_root_config_field() {
  local field_name="$1"
  local value="$2"

  # Default behavior: do not persist explicit "auto" for protocol priority.
  if [[ "$field_name" == "protocol-priority" && "$value" == "auto" ]]; then
    git config -f .gitmodules --unset-all "kog-root-config.$field_name" 2>/dev/null || true
    return 0
  fi

  # Use kog-root-config section for configuration fields
  git config -f .gitmodules "kog-root-config.$field_name" "$value"
}

# Select URL based on protocol priority and SSH availability
select_url() {
  local ssh_url="$1"
  local https_url="$2"
  local protocol_priority="${3:-auto}"

  case "$protocol_priority" in
    auto)
      # Auto-detect SSH availability
      if [[ -n "$ssh_url" ]]; then
        local host
        host=$(gith_extract_ssh_host "$ssh_url")
        if [[ -n "$host" ]] && gith_ssh_available "$host"; then
          echo "$ssh_url"
          return 0
        fi
      fi
      # Fallback to HTTPS
      echo "$https_url"
      ;;
    ssh)
      # Prefer SSH, fallback to HTTPS
      if [[ -n "$ssh_url" ]]; then
        echo "$ssh_url"
      else
        echo "$https_url"
      fi
      ;;
    https)
      # Force HTTPS
      echo "$https_url"
      ;;
    *)
      gith_error "Invalid protocol priority: $protocol_priority"
      echo "$https_url"
      ;;
  esac
}

# Validate submodule registration metadata and repair when obviously broken.
# This helps after manual submodule path rename or interrupted migration.
ensure_submodule_registration_healthy() {
  local submodule_path="$1"
  local dry_run="${2:-0}"

  local module_config=".git/modules/$submodule_path/config"
  local needs_repair=0
  local reason=""

  # Missing module metadata usually means not initialized or stale registration.
  if [[ ! -f "$module_config" ]]; then
    needs_repair=1
    reason="missing module config"
  fi

  # If core.worktree points to another path, root git status can break hard.
  if [[ "$needs_repair" -eq 0 ]]; then
    local configured_worktree=""
    configured_worktree="$(git config -f "$module_config" --get core.worktree 2>/dev/null || true)"
    if [[ -n "$configured_worktree" && "$configured_worktree" != *"$submodule_path" ]]; then
      needs_repair=1
      reason="worktree mismatch: $configured_worktree"
    fi
  fi

  # Worktree exists but not a valid git repo from this superproject perspective.
  if [[ "$needs_repair" -eq 0 ]]; then
    if ! git -C "$submodule_path" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      needs_repair=1
      reason="invalid submodule worktree"
    fi
  fi

  if [[ "$needs_repair" -eq 0 ]]; then
    return 0
  fi

  gith_log "WARN" "  Detected broken submodule registration for '$submodule_path' ($reason)"

  if [[ "$dry_run" -eq 1 ]]; then
    gith_log "INFO" "  [DRY-RUN] Would repair by deinit + re-init: $submodule_path"
    return 0
  fi

  # Rebuild registration metadata from .gitmodules authoritative state.
  git submodule deinit -f -- "$submodule_path" >/dev/null 2>&1 || true
  rm -rf ".git/modules/$submodule_path" || true
  git submodule sync -- "$submodule_path" >/dev/null 2>&1 || true
  git submodule update --init -- "$submodule_path"

  gith_log "INFO" "  Re-registered submodule: $submodule_path"
}

#------------------------------------------------------------------------------
# Command: add
#------------------------------------------------------------------------------

cmd_add() {
  local path=""
  local push_remote="origin"
  local protocol_priority="auto"
  local branch=""
  local dry_run=0

  # Associative arrays for remote URLs
  declare -A REMOTE_SSH_URLS
  declare -A REMOTE_HTTPS_URLS

  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help) usage_add; exit 0 ;;
      --path) path="$2"; shift 2 ;;
      --push-remote) push_remote="$2"; shift 2 ;;
      --protocol) protocol_priority="$2"; shift 2 ;;
      --branch) branch="$2"; shift 2 ;;
      --dry-run) dry_run=1; shift ;;
      --remote|--ssh|--https)
        # Parse remote configuration
        parse_remote_config "$@"
        # Skip processed arguments
        while [[ $# -gt 0 ]] && [[ "$1" != --path && "$1" != --push-remote && "$1" != --protocol && "$1" != --branch && "$1" != --dry-run ]]; do
          shift
        done
        ;;
      *) gith_error "Unknown option: $1"; usage_add; exit 1 ;;
    esac
  done

  # Validate required arguments
  if [[ ${#REMOTE_SSH_URLS[@]} -eq 0 && ${#REMOTE_HTTPS_URLS[@]} -eq 0 ]]; then
    gith_error "Error: At least one remote must be configured"
    usage_add
    exit 1
  fi

  # Validate that each remote has at least one URL (SSH or HTTPS)
  local all_remotes
  all_remotes=$(echo "${!REMOTE_SSH_URLS[@]} ${!REMOTE_HTTPS_URLS[@]}" | tr ' ' '\n' | sort -u)

  for remote_name in $all_remotes; do
    if [[ -z "${REMOTE_SSH_URLS[$remote_name]:-}" && -z "${REMOTE_HTTPS_URLS[$remote_name]:-}" ]]; then
      gith_error "Error: Remote '$remote_name' missing both SSH and HTTPS URL"
      exit 1
    fi
  done

  # Determine mode: root repo or submodule
  if [[ -z "$path" ]]; then
    # Root repo mode
    cmd_add_root_repo "$push_remote" "$protocol_priority" "$dry_run"
  else
    # Submodule mode
    cmd_add_submodule "$path" "$push_remote" "$protocol_priority" "$branch" "$dry_run"
  fi
}

#------------------------------------------------------------------------------
# Command: add (root repo mode)
#------------------------------------------------------------------------------

cmd_add_root_repo() {
  local push_remote="$1"
  local protocol_priority="$2"
  local dry_run="$3"

  gith_log "INFO" "Configuring root repo with multi-remote support"

  # Validate URLs
  gith_log "INFO" "Validating remote URLs..."
  for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
    local ssh_url="${REMOTE_SSH_URLS[$remote_name]}"
    local https_url="${REMOTE_HTTPS_URLS[$remote_name]}"

    # Validate URL formats
    if ! gith_validate_url "$ssh_url"; then
      gith_error "Error: Invalid SSH URL for remote '$remote_name': $ssh_url"
      exit 1
    fi

    if ! gith_validate_url "$https_url"; then
      gith_error "Error: Invalid HTTPS URL for remote '$remote_name': $https_url"
      exit 1
    fi

    gith_log "INFO" "  ✓ Remote '$remote_name' URLs validated"
  done

  # Create or update .gitmodules with kog-root-remote-* fields
  if [[ "$dry_run" -eq 1 ]]; then
    echo "[DRY-RUN] Would configure root repo remotes in .gitmodules:"
    for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
      echo "  [kog-root-remote \"$remote_name\"]"
      echo "    kog-url-ssh = ${REMOTE_SSH_URLS[$remote_name]}"
      echo "    kog-url-https = ${REMOTE_HTTPS_URLS[$remote_name]}"
    done
    echo "  kog-root-push-remote = $push_remote"
    echo "  kog-root-protocol-priority = $protocol_priority"
  else
    # Ensure .gitmodules exists
    if [[ ! -f .gitmodules ]]; then
      touch .gitmodules
      git add .gitmodules
    fi

    # Add kog-root-remote-* fields for each remote
    for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
      set_root_kog_field "$remote_name" "kog-url-ssh" "${REMOTE_SSH_URLS[$remote_name]}"
      set_root_kog_field "$remote_name" "kog-url-https" "${REMOTE_HTTPS_URLS[$remote_name]}"
      gith_log "INFO" "  Configured remote '$remote_name'"
    done

    # Add configuration fields
    set_root_config_field "push-remote" "$push_remote"
    set_root_config_field "protocol-priority" "$protocol_priority"

    gith_log "INFO" "Root repo configured with multi-remote support"
    gith_log "INFO" "  Remotes: ${!REMOTE_SSH_URLS[*]}"
    gith_log "INFO" "  Push remote: $push_remote"
    gith_log "INFO" "  Protocol priority: $protocol_priority"

    # Configure local .git/config based on SSH availability
    gith_log "INFO" "Configuring local remotes based on SSH availability..."
    cmd_sync_root_repo "$dry_run"
  fi
}

#------------------------------------------------------------------------------
# Command: add (submodule mode)
#------------------------------------------------------------------------------

cmd_add_submodule() {
  local path="$1"
  local push_remote="$2"
  local protocol_priority="$3"
  local branch="$4"
  local dry_run="$5"

  # Validate current branch (Requirement 5.1)
  local current_branch
  current_branch=$(gith_get_current_branch ".")
  if [[ -z "$current_branch" ]]; then
    gith_error "Error: Not on a branch (detached HEAD state)"
    gith_error "Please checkout a branch before adding submodules"
    exit 1
  fi
  gith_log "INFO" "Current branch: $current_branch"

  # Check for path conflicts (Requirement 5.6, 7.5)
  gith_log "INFO" "Checking for path conflicts..."
  if [[ -e "$path" ]]; then
    # Check if it's already a submodule
    if git config -f .gitmodules "submodule.$path.path" >/dev/null 2>&1; then
      gith_error "Error: Path already contains a submodule: $path"
      gith_error "Use 'git submodule update' to update existing submodule"
      exit 1
    fi

    # Check if it's a directory or file
    if [[ -d "$path" ]]; then
      gith_error "Error: Path already contains a directory: $path"
      gith_error "Please remove or rename the existing directory"
      exit 1
    elif [[ -f "$path" ]]; then
      gith_error "Error: Path already contains a file: $path"
      gith_error "Please remove or rename the existing file"
      exit 1
    fi
  fi
  gith_log "INFO" "  ✓ No path conflicts detected"

  # Validate URLs (Requirement 5.2)
  gith_log "INFO" "Validating remote URLs..."
  for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
    local ssh_url="${REMOTE_SSH_URLS[$remote_name]}"
    local https_url="${REMOTE_HTTPS_URLS[$remote_name]}"

    # Validate URL formats
    if ! gith_validate_url "$ssh_url"; then
      gith_error "Error: Invalid SSH URL for remote '$remote_name': $ssh_url"
      exit 1
    fi

    if ! gith_validate_url "$https_url"; then
      gith_error "Error: Invalid HTTPS URL for remote '$remote_name': $https_url"
      exit 1
    fi

    # Validate URL accessibility using git ls-remote
    gith_log "INFO" "  Checking accessibility of $https_url..."
    if ! git ls-remote "$https_url" HEAD >/dev/null 2>&1; then
      gith_error "Error: Remote URL not accessible: $https_url"
      gith_error "Diagnostic information:"
      gith_error "  - Check network connectivity"
      gith_error "  - Verify repository exists"
      gith_error "  - Check authentication credentials"
      exit 1
    fi
    gith_log "INFO" "  ✓ Remote '$remote_name' is accessible"
  done

  # Use HTTPS URL from first remote (or push_remote if specified) for git submodule add
  local default_url=""
  if [[ -n "${REMOTE_HTTPS_URLS[$push_remote]:-}" ]]; then
    default_url="${REMOTE_HTTPS_URLS[$push_remote]}"
  else
    # Use first available HTTPS URL
    for remote_name in "${!REMOTE_HTTPS_URLS[@]}"; do
      default_url="${REMOTE_HTTPS_URLS[$remote_name]}"
      break
    done
  fi

  if [[ -z "$default_url" ]]; then
    gith_error "Error: No HTTPS URL available"
    exit 1
  fi

  # Add submodule using git
  if [[ "$dry_run" -eq 1 ]]; then
    echo "[DRY-RUN] Would add submodule:"
    if [[ -n "$branch" ]]; then
      echo "  git submodule add -b \"$branch\" \"$default_url\" \"$path\""
    else
      echo "  git submodule add \"$default_url\" \"$path\""
    fi

    # Show kog-* fields that would be added
    for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
      echo "  git config -f .gitmodules \"submodule.$path.kog-remote-$remote_name-ssh\" \"${REMOTE_SSH_URLS[$remote_name]}\""
      echo "  git config -f .gitmodules \"submodule.$path.kog-remote-$remote_name-https\" \"${REMOTE_HTTPS_URLS[$remote_name]}\""
    done
    echo "  git config -f .gitmodules \"submodule.$path.kog-push-remote\" \"$push_remote\""
    if [[ "$protocol_priority" != "auto" ]]; then
      echo "  git config -f .gitmodules \"submodule.$path.kog-protocol-priority\" \"$protocol_priority\""
    fi
  else
    gith_log "INFO" "Adding submodule: $path"

    # Add submodule
    if [[ -n "$branch" ]]; then
      git submodule add -b "$branch" "$default_url" "$path"
    else
      git submodule add "$default_url" "$path"
    fi

    # Add kog-* extension fields for each remote
    for remote_name in "${!REMOTE_SSH_URLS[@]}"; do
      set_kog_field "$path" "kog-remote-$remote_name-ssh" "${REMOTE_SSH_URLS[$remote_name]}"
      set_kog_field "$path" "kog-remote-$remote_name-https" "${REMOTE_HTTPS_URLS[$remote_name]}"
    done

    # Add configuration fields
    set_kog_field "$path" "kog-push-remote" "$push_remote"
    # "auto" is default behavior; only persist when explicitly overridden.
    if [[ "$protocol_priority" != "auto" ]]; then
      set_kog_field "$path" "kog-protocol-priority" "$protocol_priority"
    fi

    gith_log "INFO" "Submodule added with multi-remote support"
    gith_log "INFO" "  Path: $path"
    gith_log "INFO" "  Remotes: ${!REMOTE_SSH_URLS[*]}"
    gith_log "INFO" "  Push remote: $push_remote"
    gith_log "INFO" "  Protocol priority: $protocol_priority"

    # Configure local .git/config based on SSH availability
    gith_log "INFO" "Configuring local remotes based on SSH availability..."
    cmd_sync "$path"
  fi
}

#------------------------------------------------------------------------------
# Command: sync
#------------------------------------------------------------------------------

cmd_sync() {
  local target_path=""
  local dry_run=0

  # Optional positional target path (only when first arg is not an option)
  if [[ $# -gt 0 && "${1:-}" != -* ]]; then
    target_path="$1"
    shift || true
  fi

  # Parse options
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help) usage_sync; exit 0 ;;
      --dry-run) dry_run=1; shift ;;
      *) gith_error "Unknown option: $1"; usage_sync; exit 1 ;;
    esac
  done

  # Check if we have any submodules
  local has_submodules=0
  if git config -f .gitmodules --get-regexp "submodule\..*\.path" >/dev/null 2>&1; then
    has_submodules=1
  fi

  # Check if we have root repo configuration
  local has_root_config=0
  if git config -f .gitmodules --get-regexp "kog-root-remote\..*\.kog-url-.*" >/dev/null 2>&1; then
    has_root_config=1
  fi

  # Determine mode
  if [[ -n "$target_path" ]]; then
    # Specific submodule path provided - sync that submodule
    cmd_sync_submodule "$target_path" "$dry_run"
  elif [[ "$has_submodules" -eq 1 ]]; then
    # No path provided, but submodules exist - sync all submodules
    cmd_sync_all_submodules "$dry_run"
  elif [[ "$has_root_config" -eq 1 ]]; then
    # No submodules, but root config exists - sync root repo
    cmd_sync_root_repo "$dry_run"
  else
    gith_log "INFO" "No submodules or root repo configuration found"
    return 0
  fi
}

#------------------------------------------------------------------------------
# Command: sync (root repo mode)
#------------------------------------------------------------------------------

cmd_sync_root_repo() {
  local dry_run="$1"

  gith_log "INFO" "Syncing root repo remotes..."

  # Get protocol priority
  local protocol_priority
  protocol_priority=$(get_root_config_field "protocol-priority" "auto")

  # Get all remote names into an array
  local remote_names=()
  while IFS= read -r remote_name; do
    if [[ -n "$remote_name" ]]; then
      remote_names+=("$remote_name")
    fi
  done < <(get_root_repo_remotes)

  if [[ ${#remote_names[@]} -eq 0 ]]; then
    gith_log "WARN" "No kog-root-remote-* fields found"
    return 0
  fi

  # Configure each remote
  for remote_name in "${remote_names[@]}"; do

    local ssh_url
    local https_url
    ssh_url=$(get_root_kog_field "$remote_name" "kog-url-ssh")
    https_url=$(get_root_kog_field "$remote_name" "kog-url-https")

    if [[ -z "$ssh_url" && -z "$https_url" ]]; then
      gith_log "WARN" "  Remote '$remote_name' missing both SSH and HTTPS URL, skipping"
      continue
    fi

    local direct_url
    direct_url=$(get_root_kog_field "$remote_name" "kog-url")

    # Select URL based on direct URL, protocol priority and SSH availability
    local selected_url
    if [[ -n "$direct_url" ]]; then
      selected_url="$direct_url"
    else
      selected_url=$(select_url "$ssh_url" "$https_url" "$protocol_priority")
    fi

    if [[ -z "$selected_url" ]]; then
      gith_log "WARN" "  Remote '$remote_name' has no valid URL, skipping"
      continue
    fi

    if [[ "$dry_run" -eq 1 ]]; then
      echo "  [DRY-RUN] Would configure remote '$remote_name' with URL: $selected_url"
    else
      # Add or update remote in .git/config
      if git remote | grep -qx "$remote_name" 2>/dev/null; then
        git remote set-url "$remote_name" "$selected_url"
        gith_log "INFO" "  Updated remote '$remote_name': $selected_url"
      else
        git remote add "$remote_name" "$selected_url"
        gith_log "INFO" "  Added remote '$remote_name': $selected_url"
      fi
    fi
  done

  gith_log "INFO" "Root repo sync complete"
}

#------------------------------------------------------------------------------
# Command: sync (single submodule)
#------------------------------------------------------------------------------

cmd_sync_submodule() {
  local submodule_path="$1"
  local dry_run="$2"

  gith_log "INFO" "Syncing: $submodule_path"

  ensure_submodule_registration_healthy "$submodule_path" "$dry_run"

  # Get protocol priority
  local protocol_priority
  protocol_priority=$(get_kog_field "$submodule_path" "kog-protocol-priority" "auto")

  # Get all remote names
  local remote_names
  remote_names=$(get_submodule_remotes "$submodule_path")

  if [[ -z "$remote_names" ]]; then
    gith_log "WARN" "  No kog-remote-* fields found, skipping"
    return 0
  fi

  # Ensure submodule is on the correct branch.
  # Priority:
  #   1) submodule.<path>.branch in .gitmodules
  #   2) detected default branch from tracking remote
  #   3) fallback to main (safe default)
  local target_branch
  target_branch=$(git config -f .gitmodules "submodule.$submodule_path.branch" 2>/dev/null || true)

  # Determine best tracking remote (prefer upstream)
  local tracking_remote="origin"
  if git config -f .gitmodules "submodule.$submodule_path.kog-remote-upstream-https" >/dev/null 2>&1 || \
     git config -f .gitmodules "submodule.$submodule_path.kog-remote-upstream" >/dev/null 2>&1; then
    tracking_remote="upstream"
  fi

  if [[ -z "$target_branch" ]]; then
    local detected_branch
    detected_branch="$(gith_get_default_branch "$tracking_remote" "$submodule_path" || true)"
    if [[ -n "$detected_branch" ]]; then
      target_branch="$detected_branch"
      if [[ "$dry_run" -eq 1 ]]; then
        gith_log "INFO" "  [DRY-RUN] Would set .gitmodules branch to '$target_branch' for $submodule_path"
      else
        git config -f .gitmodules "submodule.$submodule_path.branch" "$target_branch"
        gith_log "INFO" "  Set branch from remote default: $target_branch"
      fi
    else
      target_branch="main"
      gith_log "WARN" "  Could not detect default branch for $submodule_path; fallback to '$target_branch'"
    fi
  fi

  if [[ "$dry_run" -eq 1 ]]; then
    gith_log "INFO" "  [DRY-RUN] Would ensure branch '$target_branch' in $submodule_path"
  else
    gith_checkout_branch "$target_branch" "$submodule_path" "$tracking_remote"
  fi

  # Configure each remote
  while IFS= read -r remote_name; do
    if [[ -z "$remote_name" ]]; then
      continue
    fi

    local ssh_url
    local https_url
    ssh_url=$(get_kog_field "$submodule_path" "kog-remote-$remote_name-ssh")
    https_url=$(get_kog_field "$submodule_path" "kog-remote-$remote_name-https")

    if [[ -z "$ssh_url" && -z "$https_url" ]]; then
      gith_log "WARN" "  Remote '$remote_name' missing both SSH and HTTPS URL, skipping"
      continue
    fi

    local direct_url
    direct_url=$(get_kog_field "$submodule_path" "kog-remote-$remote_name")

    # Select URL based on direct URL, protocol priority and SSH availability
    local selected_url
    if [[ -n "$direct_url" ]]; then
      selected_url="$direct_url"
    else
      selected_url=$(select_url "$ssh_url" "$https_url" "$protocol_priority")
    fi

    if [[ -z "$selected_url" ]]; then
      gith_log "WARN" "  Remote '$remote_name' has no valid URL, skipping"
      continue
    fi
    local submodule_git_dir=".git/modules/$submodule_path"
    if [[ ! -d "$submodule_git_dir" ]]; then
      gith_log "WARN" "  Submodule not initialized: $submodule_path"
      continue
    fi

    if [[ "$dry_run" -eq 1 ]]; then
      echo "  [DRY-RUN] Would configure remote '$remote_name' with URL: $selected_url"
    else
      # Add or update remote
      if (cd "$submodule_path" && git remote | grep -qx "$remote_name" 2>/dev/null); then
        (cd "$submodule_path" && git remote set-url "$remote_name" "$selected_url")
        gith_log "INFO" "  Updated remote '$remote_name': $selected_url"
      else
        (cd "$submodule_path" && git remote add "$remote_name" "$selected_url")
        gith_log "INFO" "  Added remote '$remote_name': $selected_url"
      fi
    fi
  done <<< "$remote_names"
}

#------------------------------------------------------------------------------
# Command: sync (all submodules)
#------------------------------------------------------------------------------

cmd_sync_all_submodules() {
  local dry_run="$1"

  # Get list of submodules to sync
  local submodule_paths=()
  while IFS= read -r submodule_path; do
    if [[ -n "$submodule_path" ]]; then
      submodule_paths+=("$submodule_path")
    fi
  done < <(git config -f .gitmodules --get-regexp path | awk '{ print $2 }')

  if [[ ${#submodule_paths[@]} -eq 0 ]]; then
    gith_log "INFO" "No submodules to sync"
    return 0
  fi

  gith_log "INFO" "Syncing ${#submodule_paths[@]} submodule(s)..."

  # Sync each submodule
  for submodule_path in "${submodule_paths[@]}"; do
    cmd_sync_submodule "$submodule_path" "$dry_run"
  done

  gith_log "INFO" "Sync complete"
}

#------------------------------------------------------------------------------
# Command: update
#------------------------------------------------------------------------------

cmd_update() {
  local target_path=""
  local dry_run=0

  # Optional positional target path (only when first arg is not an option)
  if [[ $# -gt 0 && "${1:-}" != -* ]]; then
    target_path="$1"
    shift || true
  fi

  # Parse options
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help) usage_update; exit 0 ;;
      --dry-run) dry_run=1; shift ;;
      *) gith_error "Unknown option: $1"; usage_update; exit 1 ;;
    esac
  done

  # Get list of submodules to update
  local submodule_paths=()
  if [[ -n "$target_path" ]]; then
    submodule_paths=("$target_path")
  else
    # Get all submodules
    while IFS= read -r submodule_path; do
      if [[ -n "$submodule_path" ]]; then
        submodule_paths+=("$submodule_path")
      fi
    done < <(git config -f .gitmodules --get-regexp path | awk '{ print $2 }')
  fi

  if [[ ${#submodule_paths[@]} -eq 0 ]]; then
    gith_log "INFO" "No submodules to update"
    return 0
  fi

  gith_log "INFO" "Updating ${#submodule_paths[@]} submodule(s)..."

  # Update each submodule
  for submodule_path in "${submodule_paths[@]}"; do
    gith_log "INFO" "Updating: $submodule_path"

    # Get kog-* fields
    local ssh_url
    local https_url
    ssh_url=$(get_kog_field "$submodule_path" "kog-url-ssh")
    https_url=$(get_kog_field "$submodule_path" "kog-url-https")

    # If no kog-* fields, use standard git submodule update
    if [[ -z "$ssh_url" && -z "$https_url" ]]; then
      if [[ "$dry_run" -eq 1 ]]; then
        echo "  [DRY-RUN] Would run: git submodule update --init --recursive \"$submodule_path\""
      else
        git submodule update --init --recursive "$submodule_path"
        gith_log "INFO" "  Updated (standard mode)"
      fi
      continue
    fi

    # Try SSH first, fallback to HTTPS
    local update_success=0

    if [[ -n "$ssh_url" ]]; then
      if [[ "$dry_run" -eq 1 ]]; then
        echo "  [DRY-RUN] Would try SSH: $ssh_url"
      else
        gith_log "INFO" "  Trying SSH: $ssh_url"
        if git submodule update --init --recursive "$submodule_path" 2>/dev/null; then
          gith_log "INFO" "  Updated via SSH"
          update_success=1
        else
          gith_log "WARN" "  SSH failed, trying HTTPS fallback"
        fi
      fi
    fi

    # Fallback to HTTPS if SSH failed
    if [[ "$update_success" -eq 0 && -n "$https_url" ]]; then
      if [[ "$dry_run" -eq 1 ]]; then
        echo "  [DRY-RUN] Would try HTTPS: $https_url"
      else
        gith_log "INFO" "  Trying HTTPS: $https_url"
        if git submodule update --init --recursive "$submodule_path" 2>/dev/null; then
          gith_log "INFO" "  Updated via HTTPS"
          update_success=1
        else
          gith_error "  Failed to update via both SSH and HTTPS"
        fi
      fi
    fi
  done

  gith_log "INFO" "Update complete"
}

#------------------------------------------------------------------------------
# Main Entry Point
#------------------------------------------------------------------------------

main() {
  # Validate prerequisites
  gith_validate_prerequisites --require-git --require-git-repo --script-name "$(basename "$0")"

  if [[ $# -eq 0 ]]; then
    usage
    exit 1
  fi

  local command="$1"
  shift

  case "$command" in
    add) cmd_add "$@" ;;
    sync) cmd_sync "$@" ;;
    update) cmd_update "$@" ;;
    -h|--help) usage; exit 0 ;;
    *) gith_error "Unknown command: $command"; usage; exit 1 ;;
  esac
}

main "$@"
