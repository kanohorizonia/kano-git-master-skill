#!/usr/bin/env bash
#
# update-workspace-repos.sh - Update all repositories in workspace
#
# Purpose:
#   Update all repositories in workspace (root, registered subrepos, and unregistered subrepos)
#   with a single command. Supports manifest files or auto-discovery.
#
# Usage:
#   ./update-workspace-repos.sh [options]
#
# Options:
#   --plan-file <file>      Use native planner JSON file (deterministic operation order)
#   --manifest <file>       Use manifest file (default: auto-discover)
#   --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
#   --exclude <pattern>     Exclude path patterns
#   --remote <name>         Remote name (default: origin)
#   --max-depth <n>         Discovery max depth (default: 3)
#   --parallel <n>          Parallel updates (default: 1, sequential)
#   --continue-on-error     Continue if a repo fails
#   --dry-run              Preview mode
#   -h, --help             Show help
#
# Examples:
#   # Update all repos in current workspace
#   ./update-workspace-repos.sh
#
#   # Update using manifest file
#   ./update-workspace-repos.sh --manifest repos-manifest.json
#
#   # Update only unregistered subrepos
#   ./update-workspace-repos.sh --include-types unregistered
#
#   # Update with custom remote
#   ./update-workspace-repos.sh --remote upstream
#
#   # Continue on errors
#   ./update-workspace-repos.sh --continue-on-error
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

MANIFEST_FILE=""
PLAN_FILE=""
INCLUDE_TYPES="root,registered,unregistered"
EXCLUDE_PATTERNS=()
REMOTE_NAME="origin"
MAX_DEPTH=3
PARALLEL=1
CONTINUE_ON_ERROR=0
DRY_RUN=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Update all repositories in workspace (root, registered subrepos, and unregistered subrepos).

Options:
  --plan-file <file>      Use native planner JSON file (deterministic operation order)
  --manifest <file>       Use manifest file (default: auto-discover)
  --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
  --exclude <pattern>     Exclude path patterns (can be used multiple times)
  --remote <name>         Remote name (default: origin)
  --max-depth <n>         Discovery max depth (default: 3)
  --parallel <n>          Parallel updates (default: 1, sequential)
  --continue-on-error     Continue if a repo fails
  --dry-run              Preview mode
  -h, --help             Show help

Examples:
  # Update all repos in current workspace
  ./update-workspace-repos.sh

  # Update using manifest file
  ./update-workspace-repos.sh --manifest repos-manifest.json

  # Update only unregistered subrepos
  ./update-workspace-repos.sh --include-types unregistered

  # Update with custom remote
  ./update-workspace-repos.sh --remote upstream

  # Continue on errors
  ./update-workspace-repos.sh --continue-on-error

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
}

type_included() {
  local repo_type="$1"
  local include_types="$2"
  local type
  IFS=',' read -ra types <<< "$include_types"
  for type in "${types[@]}"; do
    case "$type" in
      submodule) type="registered" ;;
      standalone) type="unregistered" ;;
    esac
    if [[ "$repo_type" == "$type" ]]; then
      return 0
    fi
  done
  return 1
}

load_from_plan() {
  local plan_file="$1"

  if [[ ! -f "$plan_file" ]]; then
    gith_error "Plan file not found: $plan_file"
    return 1
  fi

  gith_log "INFO" "Loading update plan from: $plan_file"

  local plan_json
  plan_json="$(tr -d '\n' < "$plan_file")"

  local operations_json
  operations_json="${plan_json#*\"operations\":[}"
  operations_json="${operations_json%%],\"waves\":*}"

  if [[ "$operations_json" == "$plan_json" ]] || [[ -z "$operations_json" ]]; then
    gith_error "Invalid plan file: operations not found"
    return 1
  fi

  local repo_data=()
  local op
  while IFS= read -r op; do
    if [[ -z "$op" ]]; then
      continue
    fi

    local action
    action="$(echo "$op" | grep -o '"action":"[^"]*"' | sed 's/"action":"//;s/"$//')"
    if [[ "$action" != "update-repo" ]]; then
      continue
    fi

    local path
    local type
    path="$(echo "$op" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')"
    type="$(echo "$op" | grep -o '"type":"[^"]*"' | sed 's/"type":"//;s/"$//')"

    if [[ -n "$path" ]] && [[ -n "$type" ]]; then
      if type_included "$type" "$INCLUDE_TYPES"; then
        repo_data+=("$path|$type")
      fi
    fi
  done < <(echo "$operations_json" | grep -o '{[^}]*}')

  if [[ ${#repo_data[@]} -eq 0 ]]; then
    gith_log "WARN" "No operations found in plan after filtering"
    return 0
  fi

  printf '%s\n' "${repo_data[@]}"
}

# Load repositories from manifest file
load_from_manifest() {
  local manifest_file="$1"
  
  if [[ ! -f "$manifest_file" ]]; then
    gith_error "Manifest file not found: $manifest_file"
    return 1
  fi
  
  gith_log "INFO" "Loading repositories from manifest: $manifest_file"
  
  # Extract repos array from manifest
  local repos_json
  repos_json="$(grep -o '"repos":\[.*\]' "$manifest_file" | sed 's/"repos"://')"
  
  if [[ -z "$repos_json" ]]; then
    gith_error "Invalid manifest file: no repos found"
    return 1
  fi
  
  echo "$repos_json"
}

# Discover repositories
discover_repos() {
  local root_dir="$1"
  local max_depth="$2"
  shift 2
  local exclude_patterns=("$@")
  local stats_file=""
  local discover_mode="unknown"

  gith_log "INFO" "Auto-discovering repositories..."

  local repos_json
  stats_file="$(mktemp 2>/dev/null || true)"
  repos_json="$(GITH_DISCOVER_STATS_FILE="$stats_file" gith_discover_repos "$root_dir" "$max_depth" "${exclude_patterns[@]}")"
  discover_mode="$(sed -n 's/^mode=//p' "$stats_file" 2>/dev/null | head -n1)"
  rm -f "$stats_file" 2>/dev/null || true
  [[ -z "$discover_mode" ]] && discover_mode="unknown"
  gith_log "INFO" "Discover mode: $discover_mode"
  
  if [[ $? -ne 0 ]]; then
    gith_error "Failed to discover repositories"
    return 1
  fi
  
  echo "$repos_json"
}

# Filter repositories by type
filter_repos() {
  local repos_json="$1"
  local include_types="$2"
  
  # If include_types is "all" or contains all types, return all repos
  if [[ -z "$include_types" ]] || [[ "$include_types" == "all" ]] || [[ "$include_types" == "root,registered,unregistered" ]]; then
    echo "$repos_json"
    return 0
  fi
  
  # Parse include_types into array
  IFS=',' read -ra types <<< "$include_types"
  
  # Filter repos by type
  local filtered="["
  local first=1
  
  while IFS= read -r repo; do
    if [[ -z "$repo" ]]; then
      continue
    fi
    
    # Extract type from repo JSON
    local repo_type
    repo_type="$(echo "$repo" | grep -o '"type":"[^"]*"' | sed 's/"type":"//;s/"$//')"
    
    # Check if type is in include list
    for type in "${types[@]}"; do
      case "$type" in
        submodule) type="registered" ;;
        standalone) type="unregistered" ;;
      esac
      if [[ "$repo_type" == "$type" ]]; then
        if [[ $first -eq 0 ]]; then
          filtered+=","
        fi
        first=0
        filtered+="$repo"
        break
      fi
    done
  done < <(echo "$repos_json" | grep -o '{[^}]*}')
  
  filtered+="]"
  echo "$filtered"
}

# Update a single repository
update_single_repo() {
  local repo_path="$1"
  local remote_name="$2"
  local stash_ref=""
  local stash_created=0
  
  gith_log "INFO" "Updating: $repo_path"
  
  # Validate it's a git repo
  if ! gith_is_git_repo "$repo_path"; then
    gith_error "Not a git repository: $repo_path"
    return 1
  fi
  
  # Check if remote exists
  if ! gith_has_remote "$remote_name" "$repo_path"; then
    gith_log "WARN" "Skip: remote '$remote_name' not found in $repo_path"
    return 0
  fi
  
  # Handle uncommitted changes
  if gith_has_changes "$repo_path"; then
    stash_ref="$(gith_stash_create "$repo_path" "auto-stash-workspace-update")"
    if [[ $? -eq 0 ]] && [[ -n "$stash_ref" ]]; then
      stash_created=1
    fi
  fi
  
  # Fetch from remote
  if ! gith_fetch_remote "$remote_name" "$repo_path"; then
    gith_error "Failed to fetch from $remote_name"
    if [[ $stash_created -eq 1 ]]; then
      gith_log "INFO" "Stash preserved: $stash_ref"
    fi
    return 1
  fi
  
  # Get current branch
  local current_branch
  current_branch="$(gith_get_current_branch "$repo_path")"
  
  if [[ -z "$current_branch" ]]; then
    gith_log "WARN" "Skip: repository in detached HEAD state: $repo_path"
    if [[ $stash_created -eq 1 ]]; then
      gith_stash_pop "$repo_path" "$stash_ref"
    fi
    return 0
  fi
  
  # Check if branch exists on remote
  local target_branch="$current_branch"
  if ! gith_branch_exists_on_remote "$remote_name" "$current_branch" "$repo_path"; then
    gith_log "WARN" "Branch '$current_branch' not on remote, trying default branch..."
    
    local default_branch
    default_branch="$(gith_get_default_branch "$remote_name" "$repo_path")"
    
    if [[ -z "$default_branch" ]]; then
      gith_error "Could not detect default branch"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
      fi
      return 1
    fi
    
    target_branch="$default_branch"
  fi
  
  # Rebase onto remote branch
  gith_log "INFO" "Rebasing onto: $remote_name/$target_branch"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    gith_log "INFO" "[DRY-RUN] Would rebase onto: $remote_name/$target_branch"
  else
    if ! (cd "$repo_path" && git rebase "$remote_name/$target_branch" 2>&1); then
      gith_error "Rebase failed in $repo_path"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
      fi
      return 1
    fi
  fi
  
  # Pop stash if created
  if [[ $stash_created -eq 1 ]]; then
    if ! gith_stash_pop "$repo_path" "$stash_ref"; then
      gith_error "Failed to restore stash"
      return 1
    fi
  fi
  
  gith_log "INFO" "Updated successfully: $repo_path"
  return 0
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
      --manifest)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --manifest requires an argument"
          usage
          exit 1
        fi
        MANIFEST_FILE="$2"
        shift 2
        ;;
      --plan-file)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --plan-file requires an argument"
          usage
          exit 1
        fi
        PLAN_FILE="$2"
        shift 2
        ;;
      --include-types)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --include-types requires an argument"
          usage
          exit 1
        fi
        INCLUDE_TYPES="$2"
        shift 2
        ;;
      --exclude)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --exclude requires an argument"
          usage
          exit 1
        fi
        EXCLUDE_PATTERNS+=("$2")
        shift 2
        ;;
      --remote)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --remote requires an argument"
          usage
          exit 1
        fi
        REMOTE_NAME="$2"
        shift 2
        ;;
      --max-depth)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --max-depth requires an argument"
          usage
          exit 1
        fi
        MAX_DEPTH="$2"
        shift 2
        ;;
      --parallel)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --parallel requires an argument"
          usage
          exit 1
        fi
        PARALLEL="$2"
        shift 2
        ;;
      --continue-on-error)
        CONTINUE_ON_ERROR=1
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
        gith_error "Unexpected argument: $1"
        usage
        exit 1
        ;;
    esac
  done
  
  # Get repositories list
  local repos_json=""
  local repo_data=()

  if [[ -n "$PLAN_FILE" ]]; then
    while IFS= read -r repo_info; do
      if [[ -n "$repo_info" ]]; then
        repo_data+=("$repo_info")
      fi
    done < <(load_from_plan "$PLAN_FILE")
  elif [[ -n "$MANIFEST_FILE" ]]; then
    repos_json="$(load_from_manifest "$MANIFEST_FILE")"
  else
    # Set default exclude patterns if none provided
    if [[ ${#EXCLUDE_PATTERNS[@]} -eq 0 ]]; then
      EXCLUDE_PATTERNS=(
        "node_modules"
        ".cache"
        "build"
        "dist"
        ".venv"
        "venv"
        "__pycache__"
      )
    fi
    
    repos_json="$(discover_repos "." "$MAX_DEPTH" "${EXCLUDE_PATTERNS[@]}")"
  fi
  
  if [[ -z "$PLAN_FILE" ]]; then
    if [[ $? -ne 0 ]] || [[ -z "$repos_json" ]]; then
      gith_error "Failed to get repositories list"
      exit 1
    fi

    # Filter repositories by type
    repos_json="$(filter_repos "$repos_json" "$INCLUDE_TYPES")"

    # Extract repository paths
    while IFS= read -r repo; do
      if [[ -z "$repo" ]]; then
        continue
      fi

      local path
      path="$(echo "$repo" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')"

      if [[ -n "$path" ]]; then
        repo_data+=("$path|unknown")
      fi
    done < <(echo "$repos_json" | grep -o '{[^}]*}')
  fi

  if [[ ${#repo_data[@]} -eq 0 ]]; then
    gith_log "WARN" "No repositories found to update"
    exit 0
  fi

  gith_log "INFO" "Found ${#repo_data[@]} repositories to update"
  gith_log "INFO" "Remote: $REMOTE_NAME"
  
  # Update repositories
  local success_count=0
  local failure_count=0
  local failed_repos=()
  
  local repo_info
  for repo_info in "${repo_data[@]}"; do
    local repo_path
    repo_path="${repo_info%%|*}"
    if update_single_repo "$repo_path" "$REMOTE_NAME"; then
      success_count=$((success_count + 1))
    else
      failure_count=$((failure_count + 1))
      failed_repos+=("$repo_path")
      
      if [[ $CONTINUE_ON_ERROR -eq 0 ]]; then
        gith_error "Update failed, stopping (use --continue-on-error to continue)"
        break
      fi
    fi
  done
  
  # Display summary
  gith_log "INFO" ""
  gith_log "INFO" "Summary:"
  gith_log "INFO" "  Total repositories: ${#repo_data[@]}"
  gith_log "INFO" "  Successful: $success_count"
  gith_log "INFO" "  Failed: $failure_count"
  
  if [[ $failure_count -gt 0 ]]; then
    gith_log "ERROR" ""
    gith_log "ERROR" "Failed repositories:"
    for failed_repo in "${failed_repos[@]}"; do
      gith_log "ERROR" "  - $failed_repo"
    done
    exit 1
  fi
  
  gith_log "INFO" "All repositories updated successfully!"
  exit 0
}

# Run main function
main "$@"
