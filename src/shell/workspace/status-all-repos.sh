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
#   --repo-root <path>      Repository root/start path (default: .)
#   --include-types <types> Comma-separated repo types
#   --no-submodules         Exclude registered subrepos from discovery/output
#   --no-recursive          Disable recursive discovery (max-depth forced to 1)
#   --exclude <pattern>     Exclude path patterns
#   --max-depth <n>         Discovery max depth
#   --format <table|json|markdown> Output format (default: table)
#   --check-remote          Check remote status (slower)
#   --detail                Show recent commits per repository
#   --detail-commits <n>    Number of commits in detail mode (default: 3)
#   --detail-log <mode>     oneline|full (default: oneline)
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
REPO_ROOT="."
INCLUDE_TYPES="root,registered,unregistered"
INCLUDE_SUBMODULES=1
RECURSIVE=1
EXCLUDE_PATTERNS=()
MAX_DEPTH=3
OUTPUT_FORMAT="table"
CHECK_REMOTE=0
OUTPUT_FILE=""
DETAIL_MODE=0
DETAIL_COMMITS=3
DETAIL_LOG_MODE="oneline"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Generate comprehensive status report for all repositories.

Options:
  --manifest <file>       Use manifest file
  --repo-root <path>      Repository root/start path (default: .)
  --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
  --no-submodules         Exclude registered subrepos (alias for backward compatibility)
  --no-recursive          Disable recursive discovery (forces --max-depth 1)
  --exclude <pattern>     Exclude path patterns (can be used multiple times)
  --max-depth <n>         Discovery max depth (default: 3)
  --format <table|json|markdown> Output format (default: table)
  --check-remote          Check remote status (slower)
  --detail                Show recent commits per repository
  --detail-commits <n>    Number of commits in detail mode (default: 3)
  --detail-log <mode>     oneline|full (default: oneline)
  --output <file>         Save to file
  -h, --help             Show help

Examples:
  # Generate table report
  ./status-all-repos.sh

  # Generate JSON report
  ./status-all-repos.sh --format json

  # Check remote status (slower but more accurate)
  ./status-all-repos.sh --check-remote

  # Detail mode with 5 oneline commits
  ./status-all-repos.sh --detail --detail-commits 5

  # Exclude registered subrepos, no recursive scan
  ./status-all-repos.sh --no-submodules --no-recursive

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
  
  if [[ -z "$include_types" ]] || [[ "$include_types" == "all" ]] || [[ "$include_types" == "root,registered,unregistered" ]]; then
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

format_recent_commits() {
  local repo_path="$1"
  local commit_count="$2"
  local log_mode="$3"

  if [[ "$log_mode" == "full" ]]; then
    git -C "$repo_path" log -n "$commit_count" --date=iso --pretty=format:'%h %ad %an %s' 2>/dev/null || true
  else
    git -C "$repo_path" log -n "$commit_count" --oneline 2>/dev/null || true
  fi
}

format_detail_sections() {
  local status_data="$1"
  local details=""

  while IFS='|' read -r path branch type changes ahead status last_commit_date last_commit_msg; do
    [[ -z "$path" ]] && continue

    local commits
    commits="$(format_recent_commits "$path" "$DETAIL_COMMITS" "$DETAIL_LOG_MODE")"
    [[ -z "${commits:-}" ]] && commits="(no commits found)"

    details+=$'\n'
    details+="### $path ($type)"$'\n'
    details+="Branch: $branch"$'\n'
    details+="Recent commits ($DETAIL_COMMITS, $DETAIL_LOG_MODE):"$'\n'
    details+="$commits"$'\n'
  done <<< "$status_data"

  printf "%s" "$details"
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
  # Keep delimiter-safe for table/markdown pipe-splitting
  last_commit_msg="${last_commit_msg//|//}"
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
  printf "%-32s %-15s %-10s %-8s %-8s %-19s %-22s %-44s\n" "PATH" "BRANCH" "TYPE" "CHANGES" "UNPUSHED" "STATUS" "LAST COMMIT TIME" "LAST ONELINE"
  printf "%-32s %-15s %-10s %-8s %-8s %-19s %-22s %-44s\n" "----" "------" "----" "-------" "--------" "------" "----------------" "------------"
  
  # Print rows
  while IFS='|' read -r path branch type changes ahead status last_commit_date last_commit_msg; do
    if [[ -z "$path" ]]; then
      continue
    fi
    
    # Truncate long paths
    if [[ ${#path} -gt 30 ]]; then
      path="...${path: -27}"
    fi
    
    # Truncate long branches
    if [[ ${#branch} -gt 13 ]]; then
      branch="${branch:0:10}..."
    fi

    if [[ ${#last_commit_date} -gt 20 ]]; then
      last_commit_date="${last_commit_date:0:19}"
    fi

    if [[ ${#last_commit_msg} -gt 42 ]]; then
      last_commit_msg="${last_commit_msg:0:39}..."
    fi
    
    printf "%-32s %-15s %-10s %-8s %-8s %-19s %-22s %-44s\n" "$path" "$branch" "$type" "$changes" "$ahead" "$status" "$last_commit_date" "$last_commit_msg"
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
  
  while IFS='|' read -r path branch type changes ahead status last_commit_date last_commit_msg; do
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
    echo "- **Last commit time**: $last_commit_date"
    echo "- **Last one-line log**: $last_commit_msg"
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
      --repo-root)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --repo-root requires an argument"
          usage
          exit 1
        fi
        REPO_ROOT="$2"
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
      --no-submodules)
        INCLUDE_SUBMODULES=0
        shift
        ;;
      --no-recursive)
        RECURSIVE=0
        shift
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
      --detail)
        DETAIL_MODE=1
        shift
        ;;
      --detail-commits)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --detail-commits requires an argument"
          usage
          exit 1
        fi
        DETAIL_COMMITS="$2"
        shift 2
        ;;
      --detail-log)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --detail-log requires an argument"
          usage
          exit 1
        fi
        DETAIL_LOG_MODE="$2"
        shift 2
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

  if [[ "$DETAIL_COMMITS" =~ ^[0-9]+$ ]] && [[ "$DETAIL_COMMITS" -gt 0 ]]; then
    :
  else
    gith_error "Invalid --detail-commits value: $DETAIL_COMMITS (must be positive integer)"
    exit 1
  fi

  if [[ "$DETAIL_LOG_MODE" != "oneline" ]] && [[ "$DETAIL_LOG_MODE" != "full" ]]; then
    gith_error "Invalid --detail-log value: $DETAIL_LOG_MODE (must be oneline|full)"
    exit 1
  fi

  if [[ "$RECURSIVE" -eq 0 ]]; then
    MAX_DEPTH=1
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
    
    repos_json="$(discover_repos "$REPO_ROOT" "$MAX_DEPTH" "${EXCLUDE_PATTERNS[@]}")"
  fi
  
  if [[ $? -ne 0 ]] || [[ -z "$repos_json" ]]; then
    gith_error "Failed to get repositories list"
    exit 1
  fi
  
  # Filter repositories by type
  local effective_include_types="$INCLUDE_TYPES"
  if [[ "$INCLUDE_SUBMODULES" -eq 0 ]]; then
    if [[ "$effective_include_types" == "all" ]] || [[ "$effective_include_types" == "root,registered,unregistered" ]]; then
      effective_include_types="root,unregistered"
    else
      effective_include_types="$(echo "$effective_include_types" | tr ',' '\n' | sed '/^submodule$/d; /^registered$/d' | paste -sd, -)"
      [[ -z "$effective_include_types" ]] && effective_include_types="root,unregistered"
    fi
  fi
  repos_json="$(filter_repos "$repos_json" "$effective_include_types")"
  
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
    local branch changes ahead status last_commit_date last_commit_msg
    branch="$(echo "$status_json" | grep -o '"branch":"[^"]*"' | sed 's/"branch":"//;s/"$//')"
    changes="$(echo "$status_json" | grep -o '"uncommitted_changes":[0-9]*' | sed 's/"uncommitted_changes"://')"
    ahead="$(echo "$status_json" | grep -o '"ahead":[0-9]*' | sed 's/"ahead"://')"
    status="$(echo "$status_json" | grep -o '"status":"[^"]*"' | sed 's/"status":"//;s/"$//')"
    last_commit_date="$(echo "$status_json" | grep -o '"last_commit_date":"[^"]*"' | sed 's/"last_commit_date":"//;s/"$//')"
    last_commit_msg="$(echo "$status_json" | grep -o '"last_commit_message":"[^"]*"' | sed 's/"last_commit_message":"//;s/"$//')"
    
    # Add to status array
    if [[ -n "$status_array" ]]; then
      status_array+=$'\n'
    fi
    status_array+="$status_json"
    
    # Add to status data for table/markdown
    if [[ -n "$status_data" ]]; then
      status_data+=$'\n'
    fi
    status_data+="$repo_path|$branch|$repo_type|$changes|$ahead|$status|$last_commit_date|$last_commit_msg"
    
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

  if [[ "$DETAIL_MODE" -eq 1 ]]; then
    if [[ "$OUTPUT_FORMAT" == "json" ]]; then
      gith_log "WARN" "Detail section is not embedded in JSON output; use table or markdown for commit details."
    else
      output+=$'\n'
      output+="## Commit Details"
      output+=$'\n'
      output+="$(format_detail_sections "$status_data")"
    fi
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
