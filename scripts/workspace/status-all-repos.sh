#!/usr/bin/env bash
#
# status-all-repos.sh - Generate status report for all repositories
#
# Purpose:
#   Generate comprehensive status report for all repositories in workspace
#   showing current branch, uncommitted changes, unpushed commits, etc.
#
# Usage:
#   ./status-all-repos.sh [options]
#
# Options:
#   --manifest <file>       Use manifest file
#   --include-types <types> Comma-separated repo types
#   --exclude <pattern>     Exclude path patterns
#   --max-depth <n>         Discovery max depth
#   --format <table|json|markdown> Output format (default: table)
#   --check-remote          Check remote status (slower)
#   --output <file>         Save to file
#   -h, --help             Show help
#
# Examples:
#   # Generate table report
#   ./status-all-repos.sh
#
#   # Generate JSON report
#   ./status-all-repos.sh --format json
#
#   # Check remote status (slower but more accurate)
#   ./status-all-repos.sh --check-remote
#
#   # Save to file
#   ./status-all-repos.sh --format markdown --output status-report.md
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
INCLUDE_TYPES="root,submodule,standalone"
EXCLUDE_PATTERNS=()
MAX_DEPTH=3
OUTPUT_FORMAT="table"
CHECK_REMOTE=0
OUTPUT_FILE=""

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Generate comprehensive status report for all repositories.

Options:
  --manifest <file>       Use manifest file
  --include-types <types> Comma-separated: root,submodule,standalone (default: all)
  --exclude <pattern>     Exclude path patterns (can be used multiple times)
  --max-depth <n>         Discovery max depth (default: 3)
  --format <table|json|markdown> Output format (default: table)
  --check-remote          Check remote status (slower)
  --output <file>         Save to file
  -h, --help             Show help

Examples:
  # Generate table report
  ./status-all-repos.sh

  # Generate JSON report
  ./status-all-repos.sh --format json

  # Check remote status (slower but more accurate)
  ./status-all-repos.sh --check-remote

  # Save to file
  ./status-all-repos.sh --format markdown --output status-report.md

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
  
  if [[ -z "$include_types" ]] || [[ "$include_types" == "all" ]] || [[ "$include_types" == "root,submodule,standalone" ]]; then
    echo "$repos_json"
    return 0
  fi
  
  IFS=',' read -ra types <<< "$include_types"
  
  local filtered="["
  local first=1
  
  while IFS= read -r repo; do
    if [[ -z "$repo" ]]; then
      continue
    fi
    
    local repo_type
    repo_type="$(echo "$repo" | grep -o '"type":"[^"]*"' | sed 's/"type":"//;s/"$//')"
    
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

# Collect status for a single repository
collect_repo_status() {
  local repo_path="$1"
  local check_remote="$2"
  
  # Initialize status object
  local status_json="{"
  status_json+="\"path\":\"$repo_path\","
  
  # Get current branch
  local current_branch
  current_branch="$(gith_get_current_branch "$repo_path")"
  if [[ -z "$current_branch" ]]; then
    current_branch="(detached)"
  fi
  status_json+="\"branch\":\"$current_branch\","
  
  # Count uncommitted changes
  local changes_count
  changes_count="$(cd "$repo_path" && git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
  status_json+="\"uncommitted_changes\":$changes_count,"
  
  # Count untracked files
  local untracked_count
  untracked_count="$(cd "$repo_path" && git ls-files --others --exclude-standard 2>/dev/null | wc -l | tr -d ' ')"
  status_json+="\"untracked_files\":$untracked_count,"
  
  # Get last commit info
  local last_commit_date last_commit_msg
  last_commit_date="$(cd "$repo_path" && git log -1 --format='%ci' 2>/dev/null || echo 'N/A')"
  last_commit_msg="$(cd "$repo_path" && git log -1 --format='%s' 2>/dev/null || echo 'N/A')"
  # Escape quotes in commit message
  last_commit_msg="${last_commit_msg//\"/\\\"}"
  status_json+="\"last_commit_date\":\"$last_commit_date\","
  status_json+="\"last_commit_message\":\"$last_commit_msg\","
  
  # Check remote status if requested
  if [[ "$check_remote" -eq 1 ]] && [[ "$current_branch" != "(detached)" ]]; then
    # Get default remote
    local remote="origin"
    if gith_has_remote "$remote" "$repo_path"; then
      # Fetch quietly
      (cd "$repo_path" && git fetch "$remote" 2>/dev/null || true)
      
      # Check if remote branch exists
      if gith_branch_exists_on_remote "$remote" "$current_branch" "$repo_path"; then
        # Count commits ahead/behind
        local ahead behind
        ahead="$(cd "$repo_path" && git rev-list --count "$remote/$current_branch..HEAD" 2>/dev/null || echo '0')"
        behind="$(cd "$repo_path" && git rev-list --count "HEAD..$remote/$current_branch" 2>/dev/null || echo '0')"
        
        status_json+="\"ahead\":$ahead,"
        status_json+="\"behind\":$behind,"
        
        # Determine status
        if [[ "$ahead" -eq 0 ]] && [[ "$behind" -eq 0 ]]; then
          status_json+="\"status\":\"up-to-date\""
        elif [[ "$ahead" -gt 0 ]] && [[ "$behind" -eq 0 ]]; then
          status_json+="\"status\":\"ahead $ahead\""
        elif [[ "$ahead" -eq 0 ]] && [[ "$behind" -gt 0 ]]; then
          status_json+="\"status\":\"behind $behind\""
        else
          status_json+="\"status\":\"diverged\""
        fi
      else
        status_json+="\"ahead\":0,"
        status_json+="\"behind\":0,"
        status_json+="\"status\":\"no-remote-branch\""
      fi
    else
      status_json+="\"ahead\":0,"
      status_json+="\"behind\":0,"
      status_json+="\"status\":\"no-remote\""
    fi
  else
    status_json+="\"ahead\":0,"
    status_json+="\"behind\":0,"
    status_json+="\"status\":\"not-checked\""
  fi
  
  status_json+="}"
  echo "$status_json"
}

# Format output as table
format_as_table() {
  local status_data="$1"
  
  # Print header
  printf "%-40s %-15s %-10s %-8s %-8s %-20s\n" "PATH" "BRANCH" "TYPE" "CHANGES" "UNPUSHED" "STATUS"
  printf "%-40s %-15s %-10s %-8s %-8s %-20s\n" "----" "------" "----" "-------" "--------" "------"
  
  # Print rows
  while IFS='|' read -r path branch type changes ahead status; do
    if [[ -z "$path" ]]; then
      continue
    fi
    
    # Truncate long paths
    if [[ ${#path} -gt 38 ]]; then
      path="...${path: -35}"
    fi
    
    # Truncate long branches
    if [[ ${#branch} -gt 13 ]]; then
      branch="${branch:0:10}..."
    fi
    
    printf "%-40s %-15s %-10s %-8s %-8s %-20s\n" "$path" "$branch" "$type" "$changes" "$ahead" "$status"
  done <<< "$status_data"
}

# Format output as JSON
format_as_json() {
  local repos_json="$1"
  local status_array="$2"
  
  local output="{"
  output+="\"generated_at\":\"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
  output+="\"repos\":["
  
  local first=1
  while IFS= read -r status; do
    if [[ -z "$status" ]]; then
      continue
    fi
    
    if [[ $first -eq 0 ]]; then
      output+=","
    fi
    first=0
    
    output+="$status"
  done <<< "$status_array"
  
  output+="]"
  output+="}"
  
  echo "$output"
}

# Format output as markdown
format_as_markdown() {
  local status_data="$1"
  local total_repos="$2"
  local up_to_date="$3"
  local need_attention="$4"
  
  echo "# Repository Status Report"
  echo ""
  echo "Generated: $(date '+%Y-%m-%d %H:%M:%S')"
  echo ""
  echo "## Summary"
  echo "- Total repositories: $total_repos"
  echo "- Up-to-date: $up_to_date"
  echo "- Need attention: $need_attention"
  echo ""
  echo "## Details"
  echo ""
  
  while IFS='|' read -r path branch type changes ahead status; do
    if [[ -z "$path" ]]; then
      continue
    fi
    
    echo "### $path ($type)"
    echo "- **Branch**: $branch"
    echo "- **Status**: $status"
    if [[ "$changes" -gt 0 ]]; then
      echo "- **Uncommitted changes**: $changes files"
    fi
    if [[ "$ahead" -gt 0 ]]; then
      echo "- **Unpushed commits**: $ahead"
    fi
    echo ""
  done <<< "$status_data"
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
      --format)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --format requires an argument"
          usage
          exit 1
        fi
        OUTPUT_FORMAT="$2"
        shift 2
        ;;
      --check-remote)
        CHECK_REMOTE=1
        shift
        ;;
      --output)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --output requires an argument"
          usage
          exit 1
        fi
        OUTPUT_FILE="$2"
        shift 2
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
  
  # Validate output format
  if [[ "$OUTPUT_FORMAT" != "table" ]] && [[ "$OUTPUT_FORMAT" != "json" ]] && [[ "$OUTPUT_FORMAT" != "markdown" ]]; then
    gith_error "Invalid output format: $OUTPUT_FORMAT (must be 'table', 'json', or 'markdown')"
    exit 1
  fi
  
  # Get repositories list
  local repos_json
  if [[ -n "$MANIFEST_FILE" ]]; then
    repos_json="$(load_from_manifest "$MANIFEST_FILE")"
  else
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
  
  # Extract repository data
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
  
  gith_log "INFO" "Collecting status for ${#repo_data[@]} repositories..."
  
  # Collect status for all repositories
  local status_array=""
  local status_data=""
  local up_to_date_count=0
  local need_attention_count=0
  
  for repo_info in "${repo_data[@]}"; do
    IFS='|' read -r repo_path repo_type <<< "$repo_info"
    
    local status_json
    status_json="$(collect_repo_status "$repo_path" "$CHECK_REMOTE")"
    
    # Extract fields for formatting
    local branch changes ahead status
    branch="$(echo "$status_json" | grep -o '"branch":"[^"]*"' | sed 's/"branch":"//;s/"$//')"
    changes="$(echo "$status_json" | grep -o '"uncommitted_changes":[0-9]*' | sed 's/"uncommitted_changes"://')"
    ahead="$(echo "$status_json" | grep -o '"ahead":[0-9]*' | sed 's/"ahead"://')"
    status="$(echo "$status_json" | grep -o '"status":"[^"]*"' | sed 's/"status":"//;s/"$//')"
    
    # Add to status array
    if [[ -n "$status_array" ]]; then
      status_array+=$'\n'
    fi
    status_array+="$status_json"
    
    # Add to status data for table/markdown
    if [[ -n "$status_data" ]]; then
      status_data+=$'\n'
    fi
    status_data+="$repo_path|$branch|$repo_type|$changes|$ahead|$status"
    
    # Count status
    if [[ "$status" == "up-to-date" ]] && [[ "$changes" -eq 0 ]]; then
      up_to_date_count=$((up_to_date_count + 1))
    else
      need_attention_count=$((need_attention_count + 1))
    fi
  done
  
  # Generate output
  local output=""
  if [[ "$OUTPUT_FORMAT" == "table" ]]; then
    output="$(format_as_table "$status_data")"
  elif [[ "$OUTPUT_FORMAT" == "json" ]]; then
    output="$(format_as_json "$repos_json" "$status_array")"
  elif [[ "$OUTPUT_FORMAT" == "markdown" ]]; then
    output="$(format_as_markdown "$status_data" "${#repo_data[@]}" "$up_to_date_count" "$need_attention_count")"
  fi
  
  # Output to file or stdout
  if [[ -n "$OUTPUT_FILE" ]]; then
    echo "$output" > "$OUTPUT_FILE"
    gith_log "INFO" "Status report saved to: $OUTPUT_FILE"
  else
    echo "$output"
  fi
  
  exit 0
}

# Run main function
main "$@"
