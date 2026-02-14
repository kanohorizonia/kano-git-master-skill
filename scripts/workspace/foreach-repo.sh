#!/usr/bin/env bash
#
# foreach-repo.sh - Execute commands across all repositories
#
# Purpose:
#   Execute custom commands in all discovered repositories with clear
#   output showing which repo each result belongs to.
#
# Usage:
#   ./foreach-repo.sh <command> [options]
#
# Arguments:
#   command                 Command to execute in each repo
#
# Options:
#   --manifest <file>       Use manifest file
#   --include-types <types> Comma-separated repo types
#   --exclude <pattern>     Exclude path patterns
#   --max-depth <n>         Discovery max depth
#   --continue-on-error     Continue if command fails in a repo
#   --parallel <n>          Parallel execution (default: 1)
#   --dry-run              Preview mode
#   -h, --help             Show help
#
# Examples:
#   # Check status of all repos
#   ./foreach-repo.sh "git status --short"
#
#   # Check for unpushed commits
#   ./foreach-repo.sh "git log origin/main..HEAD --oneline"
#
#   # Create branch in all repos
#   ./foreach-repo.sh "git checkout -b feature/new-feature"
#
#   # Fetch all remotes
#   ./foreach-repo.sh "git fetch --all --prune"
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

COMMAND=""
MANIFEST_FILE=""
INCLUDE_TYPES="root,submodule,standalone"
EXCLUDE_PATTERNS=()
MAX_DEPTH=3
CONTINUE_ON_ERROR=0
PARALLEL=1
DRY_RUN=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") <command> [options]

Execute custom commands across all repositories.

Arguments:
  command                 Command to execute in each repo

Options:
  --manifest <file>       Use manifest file
  --include-types <types> Comma-separated: root,submodule,standalone (default: all)
  --exclude <pattern>     Exclude path patterns (can be used multiple times)
  --max-depth <n>         Discovery max depth (default: 3)
  --continue-on-error     Continue if command fails in a repo
  --parallel <n>          Parallel execution (default: 1, sequential)
  --dry-run              Preview mode
  -h, --help             Show help

Examples:
  # Check status of all repos
  ./foreach-repo.sh "git status --short"

  # Check for unpushed commits
  ./foreach-repo.sh "git log origin/main..HEAD --oneline"

  # Create branch in all repos
  ./foreach-repo.sh "git checkout -b feature/new-feature"

  # Fetch all remotes
  ./foreach-repo.sh "git fetch --all --prune"

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
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
  
  gith_log "INFO" "Auto-discovering repositories..."
  
  local repos_json
  repos_json="$(gith_discover_repos "$root_dir" "$max_depth" "${exclude_patterns[@]}")"
  
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
  if [[ -z "$include_types" ]] || [[ "$include_types" == "all" ]] || [[ "$include_types" == "root,submodule,standalone" ]]; then
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

# Execute command in a single repository
execute_in_repo() {
  local repo_path="$1"
  local repo_type="$2"
  local command="$3"
  
  echo ""
  echo "==> [$repo_path] ($repo_type)"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would execute: $command"
    return 0
  fi
  
  # Execute command in repo directory
  if (cd "$repo_path" && eval "$command" 2>&1); then
    return 0
  else
    local exit_code=$?
    gith_error "Command failed in $repo_path (exit code: $exit_code)"
    return $exit_code
  fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  local positional_args=()
  
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
      --max-depth)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --max-depth requires an argument"
          usage
          exit 1
        fi
        MAX_DEPTH="$2"
        shift 2
        ;;
      --continue-on-error)
        CONTINUE_ON_ERROR=1
        shift
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
        positional_args+=("$1")
        shift
        ;;
    esac
  done
  
  # Get command from positional arguments
  if [[ ${#positional_args[@]} -eq 0 ]]; then
    gith_error "Command is required"
    usage
    exit 1
  fi
  
  COMMAND="${positional_args[0]}"
  
  # Get repositories list
  local repos_json
  if [[ -n "$MANIFEST_FILE" ]]; then
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
  
  if [[ $? -ne 0 ]] || [[ -z "$repos_json" ]]; then
    gith_error "Failed to get repositories list"
    exit 1
  fi
  
  # Filter repositories by type
  repos_json="$(filter_repos "$repos_json" "$INCLUDE_TYPES")"
  
  # Extract repository paths and types
  local repo_data=()
  while IFS= read -r repo; do
    if [[ -z "$repo" ]]; then
      continue
    fi
    
    local path type
    path="$(echo "$repo" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')"
    type="$(echo "$repo" | grep -o '"type":"[^"]*"' | sed 's/"type":"//;s/"$//')"
    
    if [[ -n "$path" ]]; then
      repo_data+=("$path|$type")
    fi
  done < <(echo "$repos_json" | grep -o '{[^}]*}')
  
  if [[ ${#repo_data[@]} -eq 0 ]]; then
    gith_log "WARN" "No repositories found"
    exit 0
  fi
  
  gith_log "INFO" "Executing command in ${#repo_data[@]} repositories"
  gith_log "INFO" "Command: $COMMAND"
  
  # Execute command in all repositories
  local success_count=0
  local failure_count=0
  
  for repo_info in "${repo_data[@]}"; do
    IFS='|' read -r repo_path repo_type <<< "$repo_info"
    
    if execute_in_repo "$repo_path" "$repo_type" "$COMMAND"; then
      success_count=$((success_count + 1))
    else
      failure_count=$((failure_count + 1))
      
      if [[ $CONTINUE_ON_ERROR -eq 0 ]]; then
        gith_error "Command failed, stopping (use --continue-on-error to continue)"
        break
      fi
    fi
  done
  
  # Display summary
  echo ""
  echo "Summary: ${#repo_data[@]} repos, $success_count succeeded, $failure_count failed"
  
  if [[ $failure_count -gt 0 ]]; then
    exit 1
  fi
  
  exit 0
}

# Run main function
main "$@"
