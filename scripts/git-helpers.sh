#!/usr/bin/env bash
#
# git-helpers.sh - Shared Git helper functions library
#
# Purpose:
#   Provides common Git operations for automation scripts.
#   Vendor-agnostic design: works with any Git remote provider including
#   GitHub, GitLab, Azure Repos, Bitbucket, Gitea, Gogs, self-hosted Git
#   servers, and any other Git-compatible remote.
#
# Usage:
#   Source this file in your scripts:
#     SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#     source "$SCRIPT_DIR/git-helpers.sh"
#
# Function Naming Convention:
#   All functions use the prefix 'gith_' which stands for "git-helper"
#   (NOT "GitHub" - this library is vendor-agnostic)
#
# Design Principles:
#   - Works with any Git remote provider (not tied to specific platforms)
#   - Consistent error handling and logging
#   - Support for dry-run mode where applicable
#   - Clear, actionable error messages
#   - Cross-platform compatible (Unix shells and Git Bash on Windows)
#

set -euo pipefail

# Version
GITH_VERSION="0.1.0"

#------------------------------------------------------------------------------
# Stash Management Functions
#------------------------------------------------------------------------------

# Check if repository has uncommitted changes
# Usage: gith_has_changes [repo_path]
# Returns: 0 if changes exist, 1 if clean
gith_has_changes() {
  local repo_path="${1:-.}"
  
  # Check for any changes (staged, unstaged, or untracked)
  local status_output
  status_output="$(cd "$repo_path" && git status --porcelain 2>/dev/null)"
  local result=$?
  
  # If git status failed, return error
  if [[ $result -ne 0 ]]; then
    gith_error "Not a git repository: $repo_path"
    return 2
  fi
  
  # Return 0 if changes exist, 1 if clean
  if [[ -n "$status_output" ]]; then
    return 0
  else
    return 1
  fi
}

# Create a stash with tracking
# Usage: gith_stash_create <repo_path> <message>
# Returns: stash reference on stdout, 0 on success, 1 on failure
gith_stash_create() {
  local repo_path="${1:-.}"
  local message="${2:-auto-stash}"
  local dry_run="${DRY_RUN:-0}"
  
  # Check if there are changes to stash
  if ! gith_has_changes "$repo_path"; then
    gith_log "INFO" "No changes to stash in $repo_path"
    return 0
  fi
  
  # Create stash with message (include untracked files)
  if [[ "$dry_run" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would create stash: $message"
    printf "stash@{0}"
    return 0
  fi
  
  gith_log "INFO" "Creating stash in $repo_path: $message"
  
  if ! (cd "$repo_path" && git stash push -u -m "$message" >/dev/null 2>&1); then
    gith_error "Failed to create stash in $repo_path"
    return 1
  fi
  
  # Get the stash reference
  local stash_ref
  stash_ref="$(cd "$repo_path" && git stash list -n 1 --format='%gd' 2>/dev/null)"
  
  if [[ -z "$stash_ref" ]]; then
    gith_error "Stash created but reference not found in $repo_path"
    return 1
  fi
  
  gith_log "INFO" "Stash created: $stash_ref"
  printf "%s" "$stash_ref"
  
  return 0
}

# Pop stash with error handling
# Usage: gith_stash_pop <repo_path> <stash_ref>
# Returns: 0 on success, 1 on failure (with recovery instructions)
gith_stash_pop() {
  local repo_path="${1:-.}"
  local stash_ref="${2:-stash@{0\}}"
  local dry_run="${DRY_RUN:-0}"
  
  # Check if stash exists (use -F for fixed string matching)
  if ! (cd "$repo_path" && git stash list | grep -qF "${stash_ref}:"); then
    gith_log "WARN" "Stash not found: $stash_ref in $repo_path"
    return 0  # Not an error if stash doesn't exist
  fi
  
  # Pop stash
  if [[ "$dry_run" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would pop stash: $stash_ref"
    return 0
  fi
  
  gith_log "INFO" "Popping stash in $repo_path: $stash_ref"
  
  # Try to pop with --index to restore staged state
  if (cd "$repo_path" && git stash pop --index "$stash_ref" >/dev/null 2>&1); then
    gith_log "INFO" "Stash popped successfully"
    return 0
  fi
  
  # If pop with --index failed, try without --index
  if (cd "$repo_path" && git stash pop "$stash_ref" >/dev/null 2>&1); then
    gith_log "WARN" "Stash popped but staged state not restored"
    return 0
  fi
  
  # Pop failed - likely due to conflicts
  gith_error "Failed to pop stash: $stash_ref in $repo_path"
  gith_error "Manual recovery required:"
  gith_error "  cd $repo_path"
  gith_error "  git stash list  # Verify stash still exists"
  gith_error "  git stash show $stash_ref  # Review changes"
  gith_error "  git stash apply $stash_ref  # Apply without removing"
  gith_error "  # Resolve conflicts, then:"
  gith_error "  git stash drop $stash_ref  # Remove stash after successful apply"
  
  return 1
}

#------------------------------------------------------------------------------
# Branch Operations Functions
#------------------------------------------------------------------------------

# Get current branch name or empty if detached HEAD
# Usage: gith_get_current_branch [repo_path]
# Returns: branch name or empty string
gith_get_current_branch() {
  local repo_path="${1:-.}"
  
  # Try to get current branch using symbolic-ref
  local branch_name
  branch_name="$(cd "$repo_path" && git symbolic-ref --short HEAD 2>/dev/null)"
  local result=$?
  
  # If symbolic-ref failed, we're in detached HEAD state
  if [[ $result -ne 0 ]]; then
    gith_log "DEBUG" "Repository in detached HEAD state: $repo_path"
    echo ""
    return 0
  fi
  
  # Return the branch name
  echo "$branch_name"
  return 0
}

# Detect remote's default branch (works with any Git remote provider)
# Usage: gith_get_default_branch <remote_name> [repo_path]
# Returns: default branch name (e.g., main, master, develop)
gith_get_default_branch() {
  local remote_name="${1:-origin}"
  local repo_path="${2:-.}"
  local default_branch=""
  
  # Try symbolic-ref first (most reliable method)
  # This works with any Git remote provider
  default_branch="$(cd "$repo_path" && git symbolic-ref "refs/remotes/$remote_name/HEAD" 2>/dev/null | sed "s|^refs/remotes/$remote_name/||")"
  
  if [[ -n "$default_branch" ]]; then
    gith_log "DEBUG" "Default branch detected via symbolic-ref: $default_branch"
    echo "$default_branch"
    return 0
  fi
  
  # Fallback: try common branch names
  gith_log "DEBUG" "symbolic-ref failed, trying common branch names"
  for branch in main master develop trunk; do
    if (cd "$repo_path" && git show-ref --verify --quiet "refs/remotes/$remote_name/$branch" 2>/dev/null); then
      gith_log "DEBUG" "Default branch detected via fallback: $branch"
      echo "$branch"
      return 0
    fi
  done
  
  # No default branch found
  gith_log "WARN" "Could not detect default branch for remote: $remote_name"
  echo ""
  return 1
}

# Check if branch exists on remote (vendor-agnostic)
# Usage: gith_branch_exists_on_remote <remote_name> <branch_name> [repo_path]
# Returns: 0 if exists, 1 if not
gith_branch_exists_on_remote() {
  local remote_name="${1}"
  local branch_name="${2}"
  local repo_path="${3:-.}"
  
  # Validate required arguments
  if [[ -z "$remote_name" ]]; then
    gith_error "gith_branch_exists_on_remote: remote_name is required"
    return 2
  fi
  
  if [[ -z "$branch_name" ]]; then
    gith_error "gith_branch_exists_on_remote: branch_name is required"
    return 2
  fi
  
  # Check if the remote branch reference exists
  # This works with any Git remote provider
  if (cd "$repo_path" && git show-ref --verify --quiet "refs/remotes/$remote_name/$branch_name" 2>/dev/null); then
    gith_log "DEBUG" "Branch exists on remote: $remote_name/$branch_name"
    return 0
  else
    gith_log "DEBUG" "Branch does not exist on remote: $remote_name/$branch_name"
    return 1
  fi
}

#------------------------------------------------------------------------------
# Repository Discovery Functions
#------------------------------------------------------------------------------

# Check if directory is a Git repository
# Usage: gith_is_git_repo <path>
# Returns: 0 if git repo, 1 if not
gith_is_git_repo() {
  local path="${1:-.}"
  
  # Check if path exists
  if [[ ! -d "$path" ]]; then
    gith_log "DEBUG" "Path does not exist: $path"
    return 1
  fi
  
  # Check if it's a git repository
  if (cd "$path" && git rev-parse --git-dir >/dev/null 2>&1); then
    gith_log "DEBUG" "Valid git repository: $path"
    return 0
  else
    gith_log "DEBUG" "Not a git repository: $path"
    return 1
  fi
}

# Discover all Git repositories in directory tree
# Usage: gith_discover_repos <root_dir> <max_depth> [exclude_patterns...]
# Returns: JSON array of discovered repositories
gith_discover_repos() {
  local root_dir="${1:-.}"
  local max_depth="${2:-3}"
  shift 2
  local exclude_patterns=("$@")
  
  # Default exclude patterns if none provided
  if [[ ${#exclude_patterns[@]} -eq 0 ]]; then
    exclude_patterns=(
      "node_modules"
      ".cache"
      "build"
      "dist"
      ".venv"
      "venv"
      "__pycache__"
    )
  fi
  
  # Validate root directory
  if [[ ! -d "$root_dir" ]]; then
    gith_error "Root directory does not exist: $root_dir"
    echo "[]"
    return 1
  fi
  
  gith_log "INFO" "Discovering repositories in $root_dir (max depth: $max_depth)"
  
  # Build find command with exclude patterns
  local find_cmd="find \"$root_dir\" -maxdepth $max_depth -name .git -type d"
  
  # Collect all .git directories
  local git_dirs=()
  while IFS= read -r git_dir; do
    if [[ -z "$git_dir" ]]; then
      continue
    fi
    
    # Get repo path (parent of .git directory)
    local repo_path
    repo_path="$(dirname "$git_dir")"
    
    # Check if excluded
    local excluded=0
    for pattern in "${exclude_patterns[@]}"; do
      if gith_is_excluded "$repo_path" "$pattern"; then
        excluded=1
        break
      fi
    done
    
    if [[ $excluded -eq 1 ]]; then
      gith_log "DEBUG" "Excluded: $repo_path"
      continue
    fi
    
    git_dirs+=("$repo_path")
  done < <(eval "$find_cmd" 2>/dev/null)
  
  # Determine repo types and collect metadata
  local root_repo=""
  local submodules=()
  local standalone_repos=()
  
  # Check if root_dir itself is a git repo
  if gith_is_git_repo "$root_dir"; then
    root_repo="$root_dir"
    # Collect submodules from root
    local submodule_json
    submodule_json="$(gith_collect_submodules "$root_dir")"
    
    # Parse submodule paths
    if [[ "$submodule_json" != "[]" ]]; then
      while IFS= read -r submodule_path; do
        if [[ -n "$submodule_path" ]]; then
          submodules+=("$root_dir/$submodule_path")
        fi
      done < <(echo "$submodule_json" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')
    fi
  fi
  
  # Classify discovered repos
  for repo_path in "${git_dirs[@]}"; do
    # Skip root repo (already handled)
    if [[ "$repo_path" == "$root_repo" ]]; then
      continue
    fi
    
    # Check if it's a submodule
    local is_submodule=0
    for submodule in "${submodules[@]}"; do
      if [[ "$repo_path" == "$submodule" ]]; then
        is_submodule=1
        break
      fi
    done
    
    if [[ $is_submodule -eq 0 ]]; then
      standalone_repos+=("$repo_path")
    fi
  done
  
  # Build JSON output
  local json="["
  local first=1
  
  # Add root repo
  if [[ -n "$root_repo" ]]; then
    local metadata
    metadata="$(gith_collect_repo_metadata "$root_repo")"
    # Add type field
    metadata="${metadata%\}},\"type\":\"root\"}"
    json+="$metadata"
    first=0
  fi
  
  # Add submodules
  for submodule in "${submodules[@]}"; do
    if [[ $first -eq 0 ]]; then
      json+=","
    fi
    first=0
    
    local metadata
    metadata="$(gith_collect_repo_metadata "$submodule")"
    # Add type field
    metadata="${metadata%\}},\"type\":\"submodule\"}"
    json+="$metadata"
  done
  
  # Add standalone repos
  for standalone in "${standalone_repos[@]}"; do
    if [[ $first -eq 0 ]]; then
      json+=","
    fi
    first=0
    
    local metadata
    metadata="$(gith_collect_repo_metadata "$standalone")"
    # Add type field
    metadata="${metadata%\}},\"type\":\"standalone\"}"
    json+="$metadata"
  done
  
  json+="]"
  
  gith_log "INFO" "Discovered ${#git_dirs[@]} repositories"
  echo "$json"
  return 0
}

# Collect all submodules recursively
# Usage: gith_collect_submodules [repo_path]
# Returns: JSON array of submodule paths and metadata
gith_collect_submodules() {
  local repo_path="${1:-.}"
  local submodules=()
  
  # Check if .gitmodules exists
  if [[ ! -f "$repo_path/.gitmodules" ]]; then
    gith_log "DEBUG" "No .gitmodules file found in $repo_path"
    echo "[]"
    return 0
  fi
  
  # Get list of submodule paths
  local submodule_paths
  submodule_paths="$(cd "$repo_path" && git config --file .gitmodules --get-regexp path | awk '{ print $2 }')"
  
  if [[ -z "$submodule_paths" ]]; then
    gith_log "DEBUG" "No submodules configured in $repo_path"
    echo "[]"
    return 0
  fi
  
  # Build JSON array
  local json="["
  local first=1
  
  while IFS= read -r submodule_path; do
    if [[ -z "$submodule_path" ]]; then
      continue
    fi
    
    # Add comma separator for subsequent entries
    if [[ $first -eq 0 ]]; then
      json+=","
    fi
    first=0
    
    # Get submodule URL
    local submodule_url
    submodule_url="$(cd "$repo_path" && git config --file .gitmodules --get "submodule.$submodule_path.url" 2>/dev/null || echo "")"
    
    # Escape JSON strings
    local escaped_path="${submodule_path//\\/\\\\}"
    escaped_path="${escaped_path//\"/\\\"}"
    local escaped_url="${submodule_url//\\/\\\\}"
    escaped_url="${escaped_url//\"/\\\"}"
    
    json+="{\"path\":\"$escaped_path\",\"url\":\"$escaped_url\"}"
    
    # Recursively collect submodules of this submodule
    local full_submodule_path="$repo_path/$submodule_path"
    if [[ -d "$full_submodule_path" ]] && gith_is_git_repo "$full_submodule_path"; then
      local nested_submodules
      nested_submodules="$(gith_collect_submodules "$full_submodule_path")"
      
      # If there are nested submodules, add them with adjusted paths
      if [[ "$nested_submodules" != "[]" ]]; then
        # Parse nested submodules and prepend parent path
        local nested_json
        nested_json="$(echo "$nested_submodules" | sed 's/^\[//;s/\]$//')"
        if [[ -n "$nested_json" ]]; then
          json+=",$nested_json"
        fi
      fi
    fi
  done <<< "$submodule_paths"
  
  json+="]"
  echo "$json"
  return 0
}

# Collect repository metadata (works with any Git remote provider)
# Usage: gith_collect_repo_metadata <repo_path>
# Returns: JSON object with repo information
gith_collect_repo_metadata() {
  local repo_path="${1:-.}"
  
  # Validate it's a git repo
  if ! gith_is_git_repo "$repo_path"; then
    gith_error "Not a git repository: $repo_path"
    echo "{}"
    return 1
  fi
  
  # Get current branch (empty if detached)
  local current_branch
  current_branch="$(gith_get_current_branch "$repo_path")"
  
  # Get list of remotes (works with any Git remote provider)
  local remotes
  remotes="$(cd "$repo_path" && git remote 2>/dev/null | tr '\n' ',' | sed 's/,$//')"
  
  # Check for uncommitted changes
  local has_changes="false"
  if gith_has_changes "$repo_path"; then
    has_changes="true"
  fi
  
  # Escape JSON strings
  local escaped_path="${repo_path//\\/\\\\}"
  escaped_path="${escaped_path//\"/\\\"}"
  local escaped_branch="${current_branch//\\/\\\\}"
  escaped_branch="${escaped_branch//\"/\\\"}"
  local escaped_remotes="${remotes//\"/\\\"}"
  
  # Build JSON object
  local json="{"
  json+="\"path\":\"$escaped_path\","
  json+="\"current_branch\":\"$escaped_branch\","
  json+="\"remotes\":\"$escaped_remotes\","
  json+="\"has_changes\":$has_changes"
  json+="}"
  
  echo "$json"
  return 0
}

#------------------------------------------------------------------------------
# Remote Operations Functions (Vendor-Agnostic)
#------------------------------------------------------------------------------

# Check if remote exists (works with any Git remote provider)
# Usage: gith_has_remote <remote_name> [repo_path]
# Returns: 0 if exists, 1 if not
gith_has_remote() {
  local remote_name="${1}"
  local repo_path="${2:-.}"
  
  # Validate required argument
  if [[ -z "$remote_name" ]]; then
    gith_error "gith_has_remote: remote_name is required"
    return 2
  fi
  
  # Check if it's a git repo
  if ! gith_is_git_repo "$repo_path"; then
    gith_error "Not a git repository: $repo_path"
    return 2
  fi
  
  # Check if remote exists (works with any Git remote provider)
  if (cd "$repo_path" && git remote | grep -qx "$remote_name" 2>/dev/null); then
    gith_log "DEBUG" "Remote exists: $remote_name in $repo_path"
    return 0
  else
    gith_log "DEBUG" "Remote does not exist: $remote_name in $repo_path"
    return 1
  fi
}

# Fetch from remote with error handling (vendor-agnostic)
# Usage: gith_fetch_remote <remote_name> [repo_path]
# Returns: 0 on success, 1 on failure
gith_fetch_remote() {
  local remote_name="${1}"
  local repo_path="${2:-.}"
  local dry_run="${DRY_RUN:-0}"
  
  # Validate required argument
  if [[ -z "$remote_name" ]]; then
    gith_error "gith_fetch_remote: remote_name is required"
    return 1
  fi
  
  # Check if it's a git repo
  if ! gith_is_git_repo "$repo_path"; then
    gith_error "Not a git repository: $repo_path"
    return 1
  fi
  
  # Check if remote exists
  if ! gith_has_remote "$remote_name" "$repo_path"; then
    gith_error "Remote does not exist: $remote_name in $repo_path"
    return 1
  fi
  
  # Fetch from remote (works with any Git remote provider)
  if [[ "$dry_run" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would fetch from remote: $remote_name in $repo_path"
    return 0
  fi
  
  gith_log "INFO" "Fetching from remote: $remote_name in $repo_path"
  
  if (cd "$repo_path" && git fetch "$remote_name" --prune 2>&1); then
    gith_log "INFO" "Fetch completed successfully"
    return 0
  else
    gith_error "Failed to fetch from remote: $remote_name in $repo_path"
    gith_error "Possible causes:"
    gith_error "  - Network connectivity issues"
    gith_error "  - Authentication failure"
    gith_error "  - Remote repository not accessible"
    return 1
  fi
}

#------------------------------------------------------------------------------
# Utility Functions
#------------------------------------------------------------------------------

# Dry-run wrapper for commands
# Usage: gith_run <command> [args...]
# Behavior: If DRY_RUN=1, prints command; otherwise executes it
gith_run() {
  local dry_run="${DRY_RUN:-0}"
  
  if [[ "$dry_run" == "1" ]]; then
    # Print command in quoted format
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
    return 0
  fi
  
  # Execute command
  "$@"
  return $?
}

# Consistent logging with levels
# Usage: gith_log <level> <message>
# Levels: INFO, WARN, ERROR, DEBUG
gith_log() {
  local level="${1:-INFO}"
  local message="${2:-}"
  local timestamp
  timestamp="$(date '+%Y-%m-%d %H:%M:%S')"
  
  case "$level" in
    INFO)
      echo "[$timestamp] [INFO] $message" >&2
      ;;
    WARN)
      echo "[$timestamp] [WARN] $message" >&2
      ;;
    ERROR)
      echo "[$timestamp] [ERROR] $message" >&2
      ;;
    DEBUG)
      if [[ "${GITH_DEBUG:-0}" == "1" ]]; then
        echo "[$timestamp] [DEBUG] $message" >&2
      fi
      ;;
    *)
      echo "[$timestamp] [$level] $message" >&2
      ;;
  esac
  
  return 0
}

# Error logging (outputs to stderr)
# Usage: gith_error <message>
gith_error() {
  local message="${1:-}"
  gith_log "ERROR" "$message"
  return 0
}

# Check if path matches exclude patterns
# Usage: gith_is_excluded <path> [patterns...]
# Returns: 0 if excluded, 1 if not
gith_is_excluded() {
  local path="${1}"
  shift
  local patterns=("$@")
  
  # If no patterns provided, nothing is excluded
  if [[ ${#patterns[@]} -eq 0 ]]; then
    return 1
  fi
  
  # Check each pattern
  for pattern in "${patterns[@]}"; do
    # Simple substring match (can be enhanced with glob patterns)
    if [[ "$path" == *"$pattern"* ]]; then
      gith_log "DEBUG" "Path matches exclude pattern '$pattern': $path"
      return 0
    fi
  done
  
  # No match found
  return 1
}

#------------------------------------------------------------------------------
# Library Initialization
#------------------------------------------------------------------------------

# Log library load (only if GITH_DEBUG is set)
if [[ "${GITH_DEBUG:-0}" == "1" ]]; then
  gith_log "DEBUG" "git-helpers.sh v${GITH_VERSION} loaded"
fi
