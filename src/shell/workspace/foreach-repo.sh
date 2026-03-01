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
#   --plan-file <file>       Use native planner JSON file (deterministic operation order)
#   --command <cmd>          Explicit command string (preferred for wrappers)
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
PLAN_FILE=""
EXPLICIT_COMMAND=""
MANIFEST_FILE=""
INCLUDE_TYPES="root,registered,unregistered"
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
  --plan-file <file>       Use native planner JSON file (deterministic operation order)
  --command <cmd>          Explicit command string (preferred for wrappers)
  --manifest <file>       Use manifest file
  --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
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

  gith_log "INFO" "Loading foreach plan from: $plan_file"

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
    if [[ "$action" != "foreach" ]]; then
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
    gith_log "WARN" "No foreach operations found in plan after filtering"
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
      --plan-file)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --plan-file requires an argument"
          usage
          exit 1
        fi
        PLAN_FILE="$2"
        shift 2
        ;;
      --command)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --command requires an argument"
          usage
          exit 1
        fi
        EXPLICIT_COMMAND="$2"
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
  
  # Resolve command from explicit option or positional argument
  if [[ -n "$EXPLICIT_COMMAND" ]]; then
    COMMAND="$EXPLICIT_COMMAND"
  else
    if [[ ${#positional_args[@]} -eq 0 ]]; then
      gith_error "Command is required"
      usage
      exit 1
    fi
    COMMAND="${positional_args[0]}"
  fi
  
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

    # Extract repository paths and types
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
  fi
  
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
