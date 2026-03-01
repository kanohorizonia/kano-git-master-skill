#!/usr/bin/env bash
#
# subtree-helpers.sh - Shared helper functions for Git subtree operations
#
# This library provides common functions for subtree management scripts.
# All functions are prefixed with 'sth_' (subtree-helper).
#
# Usage:
#   source "$(dirname "${BASH_SOURCE[0]}")/subtree-helpers.sh"
#

set -euo pipefail

# Version
STH_VERSION="1.0.0"

# Detect all subtrees in repository
# Returns:
#   List of subtree prefixes (one per line)
sth_detect_subtrees() {
  git log --grep="^git-subtree-dir:" --pretty=format:"%H %s" 2>/dev/null | \
    grep -o "git-subtree-dir: [^ ]*" | \
    cut -d' ' -f2 | \
    sort -u || true
}

# Get subtree remote URL
# Args:
#   $1 - subtree prefix
# Returns:
#   Remote URL if found, empty if not
sth_get_subtree_remote() {
  local prefix="$1"
  
  git log --grep="^git-subtree-dir: $prefix\$" --pretty=format:"%H %s" 2>/dev/null | \
    head -1 | \
    grep -o "git-subtree-split: [^ ]*" | \
    cut -d' ' -f2 || true
}

# Get last subtree sync commit
# Args:
#   $1 - subtree prefix
# Returns:
#   Commit hash if found, empty if not
sth_get_last_sync_commit() {
  local prefix="$1"
  
  git log --grep="^git-subtree-dir: $prefix\$" --pretty=format:"%H" 2>/dev/null | \
    head -1 || true
}

# Get last subtree sync date
# Args:
#   $1 - subtree prefix
# Returns:
#   Date string if found, "Never" if not
sth_get_last_sync_date() {
  local prefix="$1"
  local commit
  
  commit=$(sth_get_last_sync_commit "$prefix")
  
  if [[ -n "$commit" ]]; then
    git log -1 --format="%ai" "$commit" 2>/dev/null || echo "Never"
  else
    echo "Never"
  fi
}

# Check if subtree exists
# Args:
#   $1 - subtree prefix
# Returns:
#   0 if exists, 1 if not
sth_subtree_exists() {
  local prefix="$1"
  
  [[ -d "$prefix" ]] && git log --grep="^git-subtree-dir: $prefix\$" --pretty=format:"%H" 2>/dev/null | head -1 | grep -q .
}

# Validate subtree prefix
# Args:
#   $1 - subtree prefix
# Returns:
#   0 if valid, 1 if not
sth_validate_prefix() {
  local prefix="$1"
  
  # Check if prefix is not empty
  if [[ -z "$prefix" ]]; then
    sth_error "Subtree prefix cannot be empty"
    return 1
  fi
  
  # Check if prefix doesn't start with /
  if [[ "$prefix" =~ ^/ ]]; then
    sth_error "Subtree prefix cannot start with /"
    return 1
  fi
  
  # Check if prefix doesn't end with /
  if [[ "$prefix" =~ /$ ]]; then
    sth_error "Subtree prefix cannot end with /"
    return 1
  fi
  
  return 0
}

# Validate URL
# Args:
#   $1 - URL
# Returns:
#   0 if valid, 1 if not
sth_validate_url() {
  local url="$1"
  
  # Check if URL is not empty
  if [[ -z "$url" ]]; then
    sth_error "URL cannot be empty"
    return 1
  fi
  
  # Basic URL validation (http, https, git, ssh)
  if [[ ! "$url" =~ ^(https?|git|ssh):// ]] && [[ ! "$url" =~ ^[a-zA-Z0-9_-]+@[a-zA-Z0-9._-]+: ]]; then
    sth_error "Invalid URL format: $url"
    return 1
  fi
  
  return 0
}

# List all subtrees with metadata
# Returns:
#   JSON array of subtree objects
sth_list_subtrees_json() {
  local subtrees
  subtrees=$(sth_detect_subtrees)
  
  if [[ -z "$subtrees" ]]; then
    echo "[]"
    return
  fi
  
  echo "["
  local first=1
  
  while IFS= read -r prefix; do
    if [[ "$first" -eq 0 ]]; then
      echo ","
    fi
    first=0
    
    local remote
    remote=$(sth_get_subtree_remote "$prefix")
    
    local last_sync
    last_sync=$(sth_get_last_sync_date "$prefix")
    
    local last_commit
    last_commit=$(sth_get_last_sync_commit "$prefix")
    
    echo "  {"
    echo "    \"prefix\": \"$prefix\","
    echo "    \"remote\": \"${remote:-unknown}\","
    echo "    \"last_sync\": \"$last_sync\","
    echo "    \"last_commit\": \"${last_commit:-N/A}\""
    echo -n "  }"
  done <<< "$subtrees"
  
  echo ""
  echo "]"
}

# Log message with timestamp
# Args:
#   $1 - message
sth_log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# Log error message
# Args:
#   $1 - error message
sth_error() {
  echo "[ERROR] $*" >&2
}

# Log info message
# Args:
#   $1 - info message
sth_info() {
  echo "[INFO] $*"
}

# Log warning message
# Args:
#   $1 - warning message
sth_warn() {
  echo "[WARN] $*"
}
