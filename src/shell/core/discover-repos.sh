#!/usr/bin/env bash
#
# discover-repos.sh - Discover all Git repositories in workspace
#
# Purpose:
#   Automatically discover all Git repositories in a workspace including
#   root repository, registered subrepos, and unregistered subrepos.
#
# Usage:
#   ./discover-repos.sh [options]
#
# Options:
#   --root <path>         Search root directory (default: current dir)
#   --max-depth <n>       Maximum search depth (default: 3)
#   --exclude <pattern>   Exclude path patterns (can be used multiple times)
#   --format <json|list>  Output format (default: list)
#   --no-cache            Disable local discovery cache
#   --cache-ttl <sec>     Cache TTL in seconds (default: 60)
#   --refresh-cache       Force cache refresh for this run
#   --no-incremental      Disable incremental stale-cache validation
#   --max-stale <sec>     Max stale seconds allowed for incremental reuse (default: 900)
#   --metadata-level <full|minimal> Repository metadata detail level (default: full)
#   --save <file>         Save to manifest file
#   --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
#   --dry-run            Preview mode
#   -h, --help           Show help
#
# Examples:
#   # Discover repos in current directory
#   ./discover-repos.sh
#
#   # Discover with custom depth and exclude patterns
#   ./discover-repos.sh --max-depth 5 --exclude node_modules --exclude .cache
#
#   # Output as JSON
#   ./discover-repos.sh --format json
#
#   # Save to manifest file
#   ./discover-repos.sh --save repos-manifest.json
#
#   # Only show unregistered subrepos
#   ./discover-repos.sh --include-types unregistered
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

ROOT_DIR="."
MAX_DEPTH=3
EXCLUDE_PATTERNS=()
OUTPUT_FORMAT="list"
SAVE_FILE=""
INCLUDE_TYPES="root,registered,unregistered"
DRY_RUN=0
USE_CACHE=1
CACHE_TTL_SECONDS="${GITH_DISCOVER_CACHE_TTL_SECONDS:-60}"
REFRESH_CACHE=0
INCREMENTAL=1
MAX_STALE_SECONDS="${GITH_DISCOVER_MAX_STALE_SECONDS:-900}"
METADATA_LEVEL="${GITH_DISCOVER_METADATA_LEVEL:-full}"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Discover all Git repositories in workspace (root, registered subrepos, and unregistered subrepos).

Options:
  --root <path>         Search root directory (default: current dir)
  --max-depth <n>       Maximum search depth (default: 3)
  --exclude <pattern>   Exclude path patterns (can be used multiple times)
  --format <json|list>  Output format (default: list)
  --no-cache            Disable local discovery cache
  --cache-ttl <sec>     Cache TTL in seconds (default: 60)
  --refresh-cache       Force cache refresh for this run
  --no-incremental      Disable incremental stale-cache validation
  --max-stale <sec>     Max stale seconds allowed for incremental reuse (default: 900)
  --metadata-level <full|minimal> Repository metadata detail level (default: full)
  --save <file>         Save to manifest file
  --include-types <types> Comma-separated: root,registered,unregistered (aliases: submodule,standalone)
  --dry-run            Preview mode
  -h, --help           Show help

Examples:
  # Discover repos in current directory
  ./discover-repos.sh

  # Discover with custom depth and exclude patterns
  ./discover-repos.sh --max-depth 5 --exclude node_modules --exclude .cache

  # Output as JSON
  ./discover-repos.sh --format json

  # Save to manifest file
  ./discover-repos.sh --save repos-manifest.json

  # Only show unregistered subrepos
  ./discover-repos.sh --include-types unregistered

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
}

# Filter repositories by type
filter_repos_by_type() {
  local repos_json="$1"
  local include_types="$2"
  
  # If include_types is "all" or empty, return all repos
  if [[ -z "$include_types" ]] || [[ "$include_types" == "all" ]]; then
    echo "$repos_json"
    return 0
  fi
  
  # Parse include_types into array (with backward-compatible aliases)
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

# Format output as list
format_as_list() {
  local repos_json="$1"
  
  # Parse JSON and format as list
  while IFS= read -r repo; do
    if [[ -z "$repo" ]]; then
      continue
    fi
    
    # Extract fields
    local path type branch remotes has_changes
    path="$(echo "$repo" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')"
    type="$(echo "$repo" | grep -o '"type":"[^"]*"' | sed 's/"type":"//;s/"$//')"
    branch="$(echo "$repo" | grep -o '"current_branch":"[^"]*"' | sed 's/"current_branch":"//;s/"$//')"
    remotes="$(echo "$repo" | grep -o '"remotes":"[^"]*"' | sed 's/"remotes":"//;s/"$//')"
    has_changes="$(echo "$repo" | grep -o '"has_changes":[^,}]*' | sed 's/"has_changes"://')"
    
    # Format output line
    local output="$type: $path"
    if [[ -n "$branch" ]]; then
      output+=" ($branch)"
    fi
    if [[ -n "$remotes" ]]; then
      output+=" [$remotes]"
    fi
    if [[ "$has_changes" == "true" ]]; then
      output+=" *changes*"
    fi
    
    echo "$output"
  done < <(echo "$repos_json" | grep -o '{[^}]*}')
}

# Save to manifest file
save_to_manifest() {
  local repos_json="$1"
  local save_file="$2"
  
  # Create manifest structure
  local manifest="{"
  manifest+="\"version\":\"1.0\","
  manifest+="\"workspace_root\":\"$ROOT_DIR\","
  manifest+="\"generated_at\":\"$(date -u +"%Y-%m-%dT%H:%M:%SZ")\","
  manifest+="\"repos\":$repos_json"
  manifest+="}"
  
  # Write to file
  echo "$manifest" > "$save_file"
  gith_log "INFO" "Manifest saved to: $save_file"
}

# Display summary statistics
display_summary() {
  local repos_json="$1"
  local discover_mode="${2:-unknown}"
  
  # Count repos by type
  local total root_count registered_count unregistered_count
  total="$(echo "$repos_json" | { grep -o '"type":"[^"]*"' || true; } | wc -l | tr -d ' ')"
  root_count="$(echo "$repos_json" | { grep -o '"type":"root"' || true; } | wc -l | tr -d ' ')"
  registered_count="$(echo "$repos_json" | { grep -o '"type":"registered"' || true; } | wc -l | tr -d ' ')"
  unregistered_count="$(echo "$repos_json" | { grep -o '"type":"unregistered"' || true; } | wc -l | tr -d ' ')"
  
  gith_log "INFO" ""
  gith_log "INFO" "Summary:"
  gith_log "INFO" "  Discovery source: $discover_mode"
  gith_log "INFO" "  Total repositories: $total"
  gith_log "INFO" "  Root: $root_count"
  gith_log "INFO" "  Registered subrepos: $registered_count"
  gith_log "INFO" "  Unregistered subrepos: $unregistered_count"
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
      --root)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --root requires an argument"
          usage
          exit 1
        fi
        ROOT_DIR="$2"
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
      --exclude)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --exclude requires an argument"
          usage
          exit 1
        fi
        EXCLUDE_PATTERNS+=("$2")
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
      --no-cache)
        USE_CACHE=0
        shift
        ;;
      --cache-ttl)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --cache-ttl requires an argument"
          usage
          exit 1
        fi
        CACHE_TTL_SECONDS="$2"
        shift 2
        ;;
      --refresh-cache)
        REFRESH_CACHE=1
        shift
        ;;
      --no-incremental)
        INCREMENTAL=0
        shift
        ;;
      --max-stale)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --max-stale requires an argument"
          usage
          exit 1
        fi
        MAX_STALE_SECONDS="$2"
        shift 2
        ;;
      --metadata-level)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --metadata-level requires an argument"
          usage
          exit 1
        fi
        METADATA_LEVEL="$2"
        shift 2
        ;;
      --save)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --save requires an argument"
          usage
          exit 1
        fi
        SAVE_FILE="$2"
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
  
  # Validate root directory
  if [[ ! -d "$ROOT_DIR" ]]; then
    gith_error "Root directory does not exist: $ROOT_DIR"
    exit 1
  fi
  
  # Convert to absolute path
  ROOT_DIR="$(cd "$ROOT_DIR" && pwd)"
  
  # Validate output format
  if [[ "$OUTPUT_FORMAT" != "json" ]] && [[ "$OUTPUT_FORMAT" != "list" ]]; then
    gith_error "Invalid output format: $OUTPUT_FORMAT (must be 'json' or 'list')"
    exit 1
  fi

  if ! [[ "$CACHE_TTL_SECONDS" =~ ^[0-9]+$ ]]; then
    gith_error "Invalid cache TTL: $CACHE_TTL_SECONDS (must be a non-negative integer)"
    exit 1
  fi

  if ! [[ "$MAX_STALE_SECONDS" =~ ^[0-9]+$ ]]; then
    gith_error "Invalid max stale: $MAX_STALE_SECONDS (must be a non-negative integer)"
    exit 1
  fi

  if [[ "$METADATA_LEVEL" != "full" ]] && [[ "$METADATA_LEVEL" != "minimal" ]]; then
    gith_error "Invalid metadata level: $METADATA_LEVEL (must be 'full' or 'minimal')"
    exit 1
  fi
  
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
  
  gith_log "INFO" "Discovering repositories..."
  gith_log "INFO" "Root directory: $ROOT_DIR"
  gith_log "INFO" "Max depth: $MAX_DEPTH"
  gith_log "INFO" "Exclude patterns: ${EXCLUDE_PATTERNS[*]}"
  if [[ "$USE_CACHE" -eq 1 ]]; then
    gith_log "INFO" "Cache: enabled (TTL ${CACHE_TTL_SECONDS}s)"
  else
    gith_log "INFO" "Cache: disabled"
  fi
  if [[ "$INCREMENTAL" -eq 1 ]]; then
    gith_log "INFO" "Incremental cache validation: enabled (max stale ${MAX_STALE_SECONDS}s)"
  else
    gith_log "INFO" "Incremental cache validation: disabled"
  fi
  gith_log "INFO" "Metadata level: $METADATA_LEVEL"

  if [[ "$USE_CACHE" -eq 1 ]]; then
    export GITH_DISCOVER_CACHE=1
    export GITH_DISCOVER_CACHE_TTL_SECONDS="$CACHE_TTL_SECONDS"
  else
    export GITH_DISCOVER_CACHE=0
  fi
  export GITH_DISCOVER_INCREMENTAL="$INCREMENTAL"
  export GITH_DISCOVER_MAX_STALE_SECONDS="$MAX_STALE_SECONDS"
  export GITH_DISCOVER_METADATA_LEVEL="$METADATA_LEVEL"

  if [[ "$REFRESH_CACHE" -eq 1 ]]; then
    export GITH_DISCOVER_CACHE_BUST=1
  else
    unset GITH_DISCOVER_CACHE_BUST
  fi

  # Discover repositories
  local repos_json
  local discover_mode="unknown"
  local discover_stats_file=""

  discover_stats_file="$(mktemp 2>/dev/null || true)"
  if [[ -n "$discover_stats_file" ]]; then
    export GITH_DISCOVER_STATS_FILE="$discover_stats_file"
  fi

  repos_json="$(gith_discover_repos "$ROOT_DIR" "$MAX_DEPTH" "${EXCLUDE_PATTERNS[@]}")"

  if [[ -n "$discover_stats_file" && -f "$discover_stats_file" ]]; then
    discover_mode="$(sed -n 's/^mode=//p' "$discover_stats_file" | head -n 1)"
    rm -f "$discover_stats_file" 2>/dev/null || true
  fi
  unset GITH_DISCOVER_STATS_FILE
  
  if [[ $? -ne 0 ]]; then
    gith_error "Failed to discover repositories"
    exit 1
  fi
  
  # Filter by type
  repos_json="$(filter_repos_by_type "$repos_json" "$INCLUDE_TYPES")"
  
  # Output results
  if [[ "$OUTPUT_FORMAT" == "json" ]]; then
    echo "$repos_json"
  else
    format_as_list "$repos_json"
  fi
  
  # Save to manifest if requested
  if [[ -n "$SAVE_FILE" ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
      gith_log "INFO" "[DRY-RUN] Would save manifest to: $SAVE_FILE"
    else
      save_to_manifest "$repos_json" "$SAVE_FILE"
    fi
  fi
  
  # Display summary
  display_summary "$repos_json" "$discover_mode"
  
  exit 0
}

# Run main function
main "$@"
