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
# Remote and URL Management Functions
#------------------------------------------------------------------------------

# Check if a remote repository is empty (has no references)
# Usage: gith_is_remote_empty <remote_url>
# Returns: 0 if empty, 1 if has content, 2 if not accessible
gith_is_remote_empty() {
  local remote_url="${1}"

  if [[ -z "$remote_url" ]]; then
    gith_error "Remote URL is required"
    return 2
  fi

  gith_log "DEBUG" "Checking if remote is empty: $remote_url"

  # Try to list remote references
  local refs_output
  if ! refs_output=$(git ls-remote "$remote_url" 2>&1); then
    gith_log "DEBUG" "Remote not accessible: $remote_url"
    return 2
  fi

  # Count references
  local ref_count
  ref_count=$(echo "$refs_output" | wc -l)

  if [[ "$ref_count" -eq 0 ]] || [[ -z "$refs_output" ]]; then
    gith_log "DEBUG" "Remote is empty: $remote_url"
    return 0
  else
    gith_log "DEBUG" "Remote has $ref_count reference(s): $remote_url"
    return 1
  fi
}

# Validate that SSH and HTTP URLs point to the same repository
# Usage: gith_validate_url_pair <ssh_url> <https_url>
# Returns: 0 if same repo, 1 if different, 2 on error
gith_validate_url_pair() {
  local ssh_url="${1}"
  local https_url="${2}"

  if [[ -z "$ssh_url" || -z "$https_url" ]]; then
    gith_error "Both SSH and HTTPS URLs are required"
    return 2
  fi

  gith_log "DEBUG" "Validating URL pair: SSH=$ssh_url, HTTPS=$https_url"

  # Extract repository path from URLs
  # SSH: git@github.com:user/repo.git -> user/repo
  # HTTPS: https://github.com/user/repo.git -> user/repo
  local ssh_path
  local https_path

  ssh_path=$(echo "$ssh_url" | sed -E 's|git@[^:]+:(.+)\.git$|\1|')
  https_path=$(echo "$https_url" | sed -E 's|https?://[^/]+/(.+)\.git$|\1|')

  if [[ "$ssh_path" == "$https_path" ]]; then
    gith_log "DEBUG" "URLs point to same repository: $ssh_path"
    return 0
  else
    gith_log "DEBUG" "URLs point to different repositories: SSH=$ssh_path, HTTPS=$https_path"
    return 1
  fi
}

# Check if a branch exists locally or remotely
# Usage: gith_branch_exists <branch_name> [repo_path]
# Returns: 0 if exists (local or remote), 1 if not
gith_branch_exists() {
  local branch_name="${1}"
  local repo_path="${2:-.}"

  if [[ -z "$branch_name" ]]; then
    gith_error "Branch name is required"
    return 1
  fi

  # Check local branch
  if (cd "$repo_path" && git show-ref --verify --quiet "refs/heads/$branch_name" 2>/dev/null); then
    gith_log "DEBUG" "Branch exists locally: $branch_name"
    return 0
  fi

  # Check remote branches
  if (cd "$repo_path" && git show-ref --verify --quiet "refs/remotes/*/$branch_name" 2>/dev/null); then
    gith_log "DEBUG" "Branch exists remotely: $branch_name"
    return 0
  fi

  gith_log "DEBUG" "Branch does not exist: $branch_name"
  return 1
}

# Validate Git URL format and protocol
# Usage: gith_validate_url <url>
# Returns: 0 if valid, 1 if invalid
gith_validate_url() {
  local url="${1}"

  if [[ -z "$url" ]]; then
    gith_error "URL is required"
    return 1
  fi

  # Check for supported URL patterns
  # SSH: git@host:path or ssh://git@host/path
  # HTTPS: https://host/path
  # HTTP: http://host/path
  # Local: /path or ./path or file:///path
  if [[ "$url" =~ ^(https?://|git@|ssh://|/|\./|file:///) ]]; then
    gith_log "DEBUG" "Valid URL format: $url"
    return 0
  fi

  gith_error "Invalid URL format: $url"
  return 1
}

# Validate branch name against Git rules
# Usage: gith_validate_branch_name <branch_name>
# Returns: 0 if valid, 1 if invalid
gith_validate_branch_name() {
  local branch_name="${1}"

  if [[ -z "$branch_name" ]]; then
    gith_error "Branch name is required"
    return 1
  fi

  # Git branch naming rules:
  # - Cannot contain: space, ~, ^, :, ?, *, [, \, .., @{, //
  # - Cannot start or end with /
  # - Cannot end with .lock
  # - Cannot be @

  if [[ "$branch_name" =~ [\ \~\^\:\?\*\[] ]] || \
     [[ "$branch_name" =~ \.\. ]] || \
     [[ "$branch_name" =~ @\{ ]] || \
     [[ "$branch_name" =~ // ]] || \
     [[ "$branch_name" =~ ^/ ]] || \
     [[ "$branch_name" =~ /$ ]] || \
     [[ "$branch_name" =~ \.lock$ ]] || \
     [[ "$branch_name" == "@" ]]; then
    gith_error "Invalid branch name: $branch_name"
    return 1
  fi

  gith_log "DEBUG" "Valid branch name: $branch_name"
  return 0
}

# Test SSH connectivity to a Git host
# Usage: gith_ssh_available <host> [timeout]
# Returns: 0 if SSH available, 1 if not
gith_ssh_available() {
  local host="${1}"
  local timeout="${2:-5}"

  if [[ -z "$host" ]]; then
    gith_error "Host is required"
    return 1
  fi

  gith_log "DEBUG" "Testing SSH connectivity to $host (timeout: ${timeout}s)"

  # Try SSH connection (don't execute command, just test connection)
  # Use BatchMode to avoid interactive prompts
  # Use ConnectTimeout to limit wait time
  if timeout "$timeout" ssh -T -o BatchMode=yes -o ConnectTimeout="$timeout" -o StrictHostKeyChecking=no "git@$host" 2>&1 | \
     grep -qE "successfully authenticated|Hi.*You've successfully authenticated|PTY allocation request failed"; then
    gith_log "DEBUG" "SSH available for $host"
    return 0
  fi

  gith_log "DEBUG" "SSH not available for $host"
  return 1
}

# Validate path for conflicts with existing files/directories
# Usage: gith_validate_path <path>
# Returns: 0 if path is valid (no conflicts), 1 if conflicts exist
gith_validate_path() {
  local path="${1}"

  if [[ -z "$path" ]]; then
    gith_error "Path is required"
    return 1
  fi

  # Check if path already exists
  if [[ -e "$path" ]]; then
    if [[ -d "$path" ]]; then
      gith_error "Path already contains a directory: $path"
      return 1
    elif [[ -f "$path" ]]; then
      gith_error "Path already contains a file: $path"
      return 1
    else
      gith_error "Path already exists: $path"
      return 1
    fi
  fi

  gith_log "DEBUG" "Valid path (no conflicts): $path"
  return 0
}

# Extract host from SSH URL
# Usage: gith_extract_ssh_host <ssh_url>
# Returns: host on stdout
gith_extract_ssh_host() {
  local ssh_url="${1}"

  if [[ -z "$ssh_url" ]]; then
    return 1
  fi

  # git@github.com:user/repo.git -> github.com
  # ssh://git@github.com/user/repo.git -> github.com
  if [[ "$ssh_url" =~ git@([^:]+): ]]; then
    echo "${BASH_REMATCH[1]}"
  elif [[ "$ssh_url" =~ ssh://git@([^/]+)/ ]]; then
    echo "${BASH_REMATCH[1]}"
  else
    return 1
  fi
}

#------------------------------------------------------------------------------
# Error Handling Functions
#------------------------------------------------------------------------------

# Error exit codes
readonly GITH_ERROR_VALIDATION=1
readonly GITH_ERROR_STATE=2
readonly GITH_ERROR_NETWORK=3
readonly GITH_ERROR_GIT=4

# Classify and handle errors with appropriate exit codes and recovery instructions
# Usage: gith_handle_error <error_type> <error_message> [context...]
# Error types: validation, state, network, git
# Returns: exits with appropriate error code
gith_handle_error() {
  local error_type="${1}"
  local error_message="${2}"
  shift 2
  local context=("$@")

  # Log the error
  gith_error "$error_message"

  # Provide recovery instructions based on error type
  case "$error_type" in
    validation)
      gith_error "Recovery: Please check your input and try again"
      if [[ ${#context[@]} -gt 0 ]]; then
        gith_error "Details: ${context[*]}"
      fi
      exit "$GITH_ERROR_VALIDATION"
      ;;
    state)
      gith_error "Recovery: Please resolve the state conflict and retry"
      if [[ ${#context[@]} -gt 0 ]]; then
        gith_error "Current state: ${context[*]}"
      fi
      exit "$GITH_ERROR_STATE"
      ;;
    network)
      gith_error "Recovery: Please check your network connection and credentials"
      if [[ ${#context[@]} -gt 0 ]]; then
        gith_error "Details: ${context[*]}"
      fi
      exit "$GITH_ERROR_NETWORK"
      ;;
    git)
      gith_error "Recovery: Git operation failed. See details above."
      if [[ ${#context[@]} -gt 0 ]]; then
        gith_error "Details: ${context[*]}"
      fi
      exit "$GITH_ERROR_GIT"
      ;;
    *)
      gith_error "Unknown error type: $error_type"
      exit 1
      ;;
  esac
}

# Handle stash operation failures with detailed recovery instructions
# Usage: gith_handle_stash_error <operation> <stash_ref> <repo_path>
# Operations: create, pop, apply
gith_handle_stash_error() {
  local operation="${1}"
  local stash_ref="${2:-}"
  local repo_path="${3:-.}"

  gith_error "Stash operation failed: $operation"
  gith_error ""
  gith_error "Manual recovery steps:"

  case "$operation" in
    create)
      gith_error "  1. Check repository status:"
      gith_error "     cd $repo_path"
      gith_error "     git status"
      gith_error "  2. Verify you have changes to stash"
      gith_error "  3. Try manual stash:"
      gith_error "     git stash push -u -m 'manual stash'"
      ;;
    pop|apply)
      if [[ -n "$stash_ref" ]]; then
        gith_error "  1. List available stashes:"
        gith_error "     cd $repo_path"
        gith_error "     git stash list"
        gith_error "  2. Inspect the stash:"
        gith_error "     git stash show $stash_ref"
        gith_error "  3. Try applying without removing:"
        gith_error "     git stash apply $stash_ref"
        gith_error "  4. If conflicts occur, resolve them and then:"
        gith_error "     git add <resolved-files>"
        gith_error "     git stash drop $stash_ref"
      else
        gith_error "  1. List available stashes:"
        gith_error "     cd $repo_path"
        gith_error "     git stash list"
        gith_error "  2. Apply the most recent stash:"
        gith_error "     git stash apply"
      fi
      ;;
  esac

  gith_error ""
  exit "$GITH_ERROR_GIT"
}

# Classify remote operation errors
# Usage: gith_classify_remote_error <error_output>
# Returns: error classification on stdout (network, auth, repository, unknown)
gith_classify_remote_error() {
  local error_output="${1}"

  # Network errors
  if echo "$error_output" | grep -qiE "could not resolve host|connection timed out|connection refused|network is unreachable|temporary failure in name resolution"; then
    echo "network"
    return 0
  fi

  # Authentication errors
  if echo "$error_output" | grep -qiE "permission denied|authentication failed|could not read from remote|fatal: unable to access.*403|fatal: unable to access.*401"; then
    echo "auth"
    return 0
  fi

  # Repository errors
  if echo "$error_output" | grep -qiE "repository.*not found|does not appear to be a git repository|fatal: unable to access.*404"; then
    echo "repository"
    return 0
  fi

  # Unknown error
  echo "unknown"
  return 0
}

# Handle remote operation failures with error classification
# Usage: gith_handle_remote_error <operation> <remote_url> <error_output>
# Operations: clone, fetch, push, pull
gith_handle_remote_error() {
  local operation="${1}"
  local remote_url="${2}"
  local error_output="${3}"

  local error_class
  error_class="$(gith_classify_remote_error "$error_output")"

  gith_error "Remote operation failed: $operation"
  gith_error "Remote URL: $remote_url"
  gith_error ""

  case "$error_class" in
    network)
      gith_error "Error type: Network connectivity issue"
      gith_error "Recovery steps:"
      gith_error "  1. Check your internet connection"
      gith_error "  2. Verify the remote host is accessible:"
      gith_error "     ping $(echo "$remote_url" | sed -E 's|.*@([^:/]+).*|\1|')"
      gith_error "  3. Check if a firewall is blocking the connection"
      gith_error "  4. Try again in a few moments"
      exit "$GITH_ERROR_NETWORK"
      ;;
    auth)
      gith_error "Error type: Authentication failure"
      gith_error "Recovery steps:"
      gith_error "  1. Verify your credentials are correct"
      gith_error "  2. For SSH URLs, check your SSH key:"
      gith_error "     ssh -T git@$(echo "$remote_url" | sed -E 's|.*@([^:/]+).*|\1|')"
      gith_error "  3. For HTTPS URLs, check your username/password or token"
      gith_error "  4. Verify you have permission to access this repository"
      exit "$GITH_ERROR_NETWORK"
      ;;
    repository)
      gith_error "Error type: Repository not found or inaccessible"
      gith_error "Recovery steps:"
      gith_error "  1. Verify the repository URL is correct"
      gith_error "  2. Check if the repository exists"
      gith_error "  3. Verify you have permission to access this repository"
      gith_error "  4. For private repositories, ensure you're authenticated"
      exit "$GITH_ERROR_STATE"
      ;;
    unknown)
      gith_error "Error type: Unknown error"
      gith_error "Error details:"
      gith_error "$error_output"
      gith_error ""
      gith_error "Recovery steps:"
      gith_error "  1. Review the error message above"
      gith_error "  2. Check Git documentation for this error"
      gith_error "  3. Try the operation manually to get more details"
      exit "$GITH_ERROR_GIT"
      ;;
  esac
}

# Log operation start with timestamp
# Usage: gith_log_operation_start <operation> [details...]
gith_log_operation_start() {
  local operation="${1}"
  shift
  local details=("$@")

  gith_log "INFO" "=== Operation started: $operation ==="
  if [[ ${#details[@]} -gt 0 ]]; then
    gith_log "INFO" "Details: ${details[*]}"
  fi
}

# Log operation result with timestamp
# Usage: gith_log_operation_result <operation> <result> [details...]
# Result: success, failure, skipped
gith_log_operation_result() {
  local operation="${1}"
  local result="${2}"
  shift 2
  local details=("$@")

  case "$result" in
    success)
      gith_log "INFO" "=== Operation completed: $operation [SUCCESS] ==="
      ;;
    failure)
      gith_log "ERROR" "=== Operation failed: $operation [FAILURE] ==="
      ;;
    skipped)
      gith_log "INFO" "=== Operation skipped: $operation [SKIPPED] ==="
      ;;
    *)
      gith_log "INFO" "=== Operation ended: $operation [$result] ==="
      ;;
  esac

  if [[ ${#details[@]} -gt 0 ]]; then
    gith_log "INFO" "Details: ${details[*]}"
  fi
}

#------------------------------------------------------------------------------
# Prerequisite Validation Functions
#------------------------------------------------------------------------------

# Validate script prerequisites (Git installation, required arguments, Git repo)
# Usage: gith_validate_prerequisites [options]
# Options:
#   --require-git           Check if Git is installed (default: true)
#   --require-git-repo      Check if current directory is in a Git repository
#   --required-args <args>  Space-separated list of required argument names
#   --script-name <name>    Script name for error messages
# Returns: 0 if all prerequisites met, exits with error code otherwise
gith_validate_prerequisites() {
  local require_git=1
  local require_git_repo=0
  local required_args=()
  local script_name="${0##*/}"

  # Parse options
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --require-git)
        require_git=1
        shift
        ;;
      --no-require-git)
        require_git=0
        shift
        ;;
      --require-git-repo)
        require_git_repo=1
        shift
        ;;
      --required-args)
        IFS=' ' read -ra required_args <<< "$2"
        shift 2
        ;;
      --script-name)
        script_name="$2"
        shift 2
        ;;
      *)
        gith_error "Unknown option: $1"
        return 1
        ;;
    esac
  done

  local prerequisites_met=1
  local missing_prerequisites=()

  # Check if Git is installed
  if [[ "$require_git" == "1" ]]; then
    if ! command -v git >/dev/null 2>&1; then
      missing_prerequisites+=("Git is not installed")
      prerequisites_met=0
    fi
  fi

  # Check if current directory is in a Git repository
  if [[ "$require_git_repo" == "1" ]]; then
    if ! git rev-parse --git-dir >/dev/null 2>&1; then
      missing_prerequisites+=("Current directory is not in a Git repository")
      prerequisites_met=0
    fi
  fi

  # Report missing prerequisites
  if [[ "$prerequisites_met" == "0" ]]; then
    gith_error "Missing prerequisites for $script_name:"
    for prereq in "${missing_prerequisites[@]}"; do
      gith_error "  - $prereq"
    done
    gith_error ""
    gith_error "Recovery steps:"

    # Provide specific recovery instructions
    for prereq in "${missing_prerequisites[@]}"; do
      if [[ "$prereq" == *"Git is not installed"* ]]; then
        gith_error "  Install Git:"
        gith_error "    - On Ubuntu/Debian: sudo apt-get install git"
        gith_error "    - On macOS: brew install git"
        gith_error "    - On Windows: Download from https://git-scm.com/"
        gith_error "    - Verify installation: git --version"
      elif [[ "$prereq" == *"not in a Git repository"* ]]; then
        gith_error "  Initialize or navigate to a Git repository:"
        gith_error "    - Initialize new repo: git init"
        gith_error "    - Clone existing repo: git clone <url>"
        gith_error "    - Navigate to existing repo: cd /path/to/repo"
      fi
    done

    exit "$GITH_ERROR_VALIDATION"
  fi

  gith_log "DEBUG" "All prerequisites met for $script_name"
  return 0
}

#------------------------------------------------------------------------------
# Library Initialization
#------------------------------------------------------------------------------

# Log library load (only if GITH_DEBUG is set)
if [[ "${GITH_DEBUG:-0}" == "1" ]]; then
  gith_log "DEBUG" "git-helpers.sh v${GITH_VERSION} loaded"
fi
