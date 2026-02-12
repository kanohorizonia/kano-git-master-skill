#!/usr/bin/env bash
#
# version-helpers.sh - Version information extraction helpers
#
# Purpose:
#   Extract version information from Git, git-p4, and git-svn repositories
#   Provides unified interface for different VCS backends
#
# Usage:
#   Source this file in your scripts:
#     SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#     source "$SCRIPT_DIR/../lib/version-helpers.sh"
#
# Functions:
#   - get_git_version_info()      - Get standard Git version info
#   - get_p4_version_info()       - Get Perforce version info from git-p4
#   - get_svn_version_info()      - Get SVN version info from git-svn
#   - detect_vcs_type()           - Detect VCS type (git, git-p4, git-svn)
#   - get_version_info()          - Auto-detect and get version info
#   - export_version_vars()       - Export version info as environment variables
#

set -euo pipefail

# Version
readonly VH_VERSION="1.0.0"

#------------------------------------------------------------------------------
# Standard Git Version Info
#------------------------------------------------------------------------------

# Get standard Git version information
# Returns: JSON object with version info
get_git_version_info() {
  local repo_path="${1:-.}"
  
  if [[ ! -d "$repo_path/.git" ]]; then
    echo "Error: Not a git repository: $repo_path" >&2
    return 1
  fi
  
  pushd "$repo_path" >/dev/null
  
  local hash_short hash_full branch revision tag dirty
  
  # Git hash (short and full)
  hash_short=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
  hash_full=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
  
  # Branch name
  branch=$(git symbolic-ref --short HEAD 2>/dev/null || echo "detached")
  
  # Revision count (first-parent only)
  revision=$(git rev-list --count --first-parent HEAD 2>/dev/null || echo "0")
  
  # Latest tag
  tag=$(git describe --tags --abbrev=0 2>/dev/null || echo "none")
  
  # Dirty flag (uncommitted changes)
  if git diff-index --quiet HEAD -- 2>/dev/null; then
    dirty="false"
  else
    dirty="true"
  fi
  
  popd >/dev/null
  
  # Output JSON
  cat <<EOF
{
  "vcs_type": "git",
  "hash_short": "$hash_short",
  "hash_full": "$hash_full",
  "branch": "$branch",
  "revision": "$revision",
  "tag": "$tag",
  "dirty": $dirty
}
EOF
}

#------------------------------------------------------------------------------
# Git-P4 Version Info
#------------------------------------------------------------------------------

# Extract git-p4 metadata from commit message
# Format: [git-p4: depot-paths = "//DepotName/StreamName/Project/": change = 12345]
get_p4_version_info() {
  local repo_path="${1:-.}"
  
  if [[ ! -d "$repo_path/.git" ]]; then
    echo "Error: Not a git repository: $repo_path" >&2
    return 1
  fi
  
  pushd "$repo_path" >/dev/null
  
  # Get latest commit message
  local commit_msg
  commit_msg=$(git log -1 --format=%B 2>/dev/null || echo "")
  
  # Check if this is a git-p4 repo
  if ! echo "$commit_msg" | grep -q "^\[git-p4:"; then
    echo "Error: Not a git-p4 repository (no git-p4 metadata found)" >&2
    popd >/dev/null
    return 1
  fi
  
  # Extract git-p4 metadata line
  local p4_metadata
  p4_metadata=$(echo "$commit_msg" | grep "^\[git-p4:" | head -1)
  
  # Parse depot path, stream, project, and change number
  local depot stream project branch change_num
  
  # Extract depot-paths value
  # Format: depot-paths = "//DepotName/StreamName/Project/"
  local depot_path
  depot_path=$(echo "$p4_metadata" | sed -n 's/.*depot-paths = "\([^"]*\)".*/\1/p')
  
  # Parse depot path components
  # //DepotName/StreamName/Project/ -> DepotName, StreamName, Project
  depot=$(echo "$depot_path" | awk -F'/' '{print $3}')
  stream=$(echo "$depot_path" | awk -F'/' '{print $4}')
  project=$(echo "$depot_path" | awk -F'/' '{print $5}')
  
  # Branch is typically the stream name
  branch="$stream"
  
  # Extract change number
  # Format: change = 12345
  change_num=$(echo "$p4_metadata" | sed -n 's/.*change = \([0-9]*\).*/\1/p')
  
  # Also get standard Git info
  local hash_short hash_full git_branch revision
  hash_short=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
  hash_full=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
  git_branch=$(git symbolic-ref --short HEAD 2>/dev/null || echo "detached")
  revision=$(git rev-list --count --first-parent HEAD 2>/dev/null || echo "0")
  
  popd >/dev/null
  
  # Output JSON
  cat <<EOF
{
  "vcs_type": "git-p4",
  "hash_short": "$hash_short",
  "hash_full": "$hash_full",
  "git_branch": "$git_branch",
  "git_revision": "$revision",
  "p4_depot": "$depot",
  "p4_stream": "$stream",
  "p4_project": "$project",
  "p4_branch": "$branch",
  "p4_change": "$change_num"
}
EOF
}

#------------------------------------------------------------------------------
# Git-SVN Version Info
#------------------------------------------------------------------------------

# Extract git-svn metadata from commit message
# Format: git-svn-id: https://svn.example.com/repo/trunk@12345 uuid
get_svn_version_info() {
  local repo_path="${1:-.}"
  
  if [[ ! -d "$repo_path/.git" ]]; then
    echo "Error: Not a git repository: $repo_path" >&2
    return 1
  fi
  
  pushd "$repo_path" >/dev/null
  
  # Get latest commit message
  local commit_msg
  commit_msg=$(git log -1 --format=%B 2>/dev/null || echo "")
  
  # Check if this is a git-svn repo
  if ! echo "$commit_msg" | grep -q "^git-svn-id:"; then
    echo "Error: Not a git-svn repository (no git-svn-id found)" >&2
    popd >/dev/null
    return 1
  fi
  
  # Extract git-svn-id line
  local svn_metadata
  svn_metadata=$(echo "$commit_msg" | grep "^git-svn-id:" | head -1)
  
  # Parse SVN URL, revision, and UUID
  # Format: git-svn-id: https://svn.example.com/repo/trunk@12345 uuid
  local svn_url svn_revision svn_uuid svn_branch
  
  # Extract URL and revision
  # https://svn.example.com/repo/trunk@12345
  local url_with_rev
  url_with_rev=$(echo "$svn_metadata" | awk '{print $2}')
  
  # Split URL and revision
  svn_url=$(echo "$url_with_rev" | sed 's/@[0-9]*$//')
  svn_revision=$(echo "$url_with_rev" | sed -n 's/.*@\([0-9]*\)$/\1/p')
  
  # Extract UUID
  svn_uuid=$(echo "$svn_metadata" | awk '{print $3}')
  
  # Extract branch from URL
  # trunk -> trunk
  # branches/feature -> feature
  # tags/v1.0 -> v1.0
  if echo "$svn_url" | grep -q "/trunk$"; then
    svn_branch="trunk"
  elif echo "$svn_url" | grep -q "/branches/"; then
    svn_branch=$(echo "$svn_url" | sed -n 's|.*/branches/\([^/]*\).*|\1|p')
  elif echo "$svn_url" | grep -q "/tags/"; then
    svn_branch=$(echo "$svn_url" | sed -n 's|.*/tags/\([^/]*\).*|\1|p')
  else
    svn_branch="unknown"
  fi
  
  # Also get standard Git info
  local hash_short hash_full git_branch git_revision
  hash_short=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
  hash_full=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
  git_branch=$(git symbolic-ref --short HEAD 2>/dev/null || echo "detached")
  git_revision=$(git rev-list --count --first-parent HEAD 2>/dev/null || echo "0")
  
  popd >/dev/null
  
  # Output JSON
  cat <<EOF
{
  "vcs_type": "git-svn",
  "hash_short": "$hash_short",
  "hash_full": "$hash_full",
  "git_branch": "$git_branch",
  "git_revision": "$git_revision",
  "svn_url": "$svn_url",
  "svn_revision": "$svn_revision",
  "svn_branch": "$svn_branch",
  "svn_uuid": "$svn_uuid"
}
EOF
}

#------------------------------------------------------------------------------
# VCS Type Detection
#------------------------------------------------------------------------------

# Detect VCS type (git, git-p4, git-svn)
detect_vcs_type() {
  local repo_path="${1:-.}"
  
  if [[ ! -d "$repo_path/.git" ]]; then
    echo "none"
    return 1
  fi
  
  pushd "$repo_path" >/dev/null
  
  # Get latest commit message
  local commit_msg
  commit_msg=$(git log -1 --format=%B 2>/dev/null || echo "")
  
  # Check for git-p4
  if echo "$commit_msg" | grep -q "^\[git-p4:"; then
    echo "git-p4"
    popd >/dev/null
    return 0
  fi
  
  # Check for git-svn
  if echo "$commit_msg" | grep -q "^git-svn-id:"; then
    echo "git-svn"
    popd >/dev/null
    return 0
  fi
  
  # Standard git
  echo "git"
  popd >/dev/null
  return 0
}

#------------------------------------------------------------------------------
# Unified Version Info
#------------------------------------------------------------------------------

# Get version info (auto-detect VCS type)
get_version_info() {
  local repo_path="${1:-.}"
  local vcs_type
  
  vcs_type=$(detect_vcs_type "$repo_path")
  
  case "$vcs_type" in
    git-p4)
      get_p4_version_info "$repo_path"
      ;;
    git-svn)
      get_svn_version_info "$repo_path"
      ;;
    git)
      get_git_version_info "$repo_path"
      ;;
    *)
      echo "Error: Unknown VCS type: $vcs_type" >&2
      return 1
      ;;
  esac
}

#------------------------------------------------------------------------------
# Environment Variable Export
#------------------------------------------------------------------------------

# Export version info as environment variables
# Usage: eval "$(export_version_vars [repo_path] [revision_offset])"
# Arguments:
#   repo_path         Repository path (default: current directory)
#   revision_offset   Offset to add to revision number (default: 0)
#                     Example: offset=-500000 converts revision 500300 to 300
export_version_vars() {
  local repo_path="${1:-.}"
  local revision_offset="${2:-0}"
  local vcs_type
  
  vcs_type=$(detect_vcs_type "$repo_path")
  
  case "$vcs_type" in
    git-p4)
      # Export git-p4 variables
      pushd "$repo_path" >/dev/null
      
      local commit_msg p4_metadata depot_path revision
      commit_msg=$(git log -1 --format=%B 2>/dev/null || echo "")
      p4_metadata=$(echo "$commit_msg" | grep "^\[git-p4:" | head -1)
      depot_path=$(echo "$p4_metadata" | sed -n 's/.*depot-paths = "\([^"]*\)".*/\1/p')
      
      # Calculate revision with offset
      revision=$(git rev-list --count --first-parent HEAD)
      revision=$((revision + revision_offset))
      
      echo "export PROJECT_REVISION_HASH_SHORT=$(git rev-parse --short HEAD)"
      echo "export PROJECT_REVISION_HASH=$(git rev-parse HEAD)"
      echo "export PROJECT_BRANCH=$(git symbolic-ref --short HEAD 2>/dev/null || echo 'detached')"
      echo "export PROJECT_REVISION=$revision"
      echo "export PROJECT_REVISION_OFFSET=$revision_offset"
      echo "export PROJECT_DEPOT=$(echo '$depot_path' | awk -F'/' '{print $3}')"
      echo "export PROJECT_P4_STREAM=$(echo '$depot_path' | awk -F'/' '{print $4}')"
      echo "export PROJECT_P4_PROJECT=$(echo '$depot_path' | awk -F'/' '{print $5}')"
      echo "export PROJECT_P4_CHANGE=$(echo '$p4_metadata' | sed -n 's/.*change = \([0-9]*\).*/\1/p')"
      echo "export PROJECT_VCS_TYPE='git-p4'"
      
      popd >/dev/null
      ;;
      
    git-svn)
      # Export git-svn variables
      pushd "$repo_path" >/dev/null
      
      local commit_msg svn_metadata url_with_rev svn_url svn_revision svn_branch revision
      commit_msg=$(git log -1 --format=%B 2>/dev/null || echo "")
      svn_metadata=$(echo "$commit_msg" | grep "^git-svn-id:" | head -1)
      url_with_rev=$(echo "$svn_metadata" | awk '{print $2}')
      svn_url=$(echo "$url_with_rev" | sed 's/@[0-9]*$//')
      svn_revision=$(echo "$url_with_rev" | sed -n 's/.*@\([0-9]*\)$/\1/p')
      
      # Extract branch
      if echo "$svn_url" | grep -q "/trunk$"; then
        svn_branch="trunk"
      elif echo "$svn_url" | grep -q "/branches/"; then
        svn_branch=$(echo "$svn_url" | sed -n 's|.*/branches/\([^/]*\).*|\1|p')
      elif echo "$svn_url" | grep -q "/tags/"; then
        svn_branch=$(echo "$svn_url" | sed -n 's|.*/tags/\([^/]*\).*|\1|p')
      else
        svn_branch="unknown"
      fi
      
      # Calculate revision with offset
      revision=$(git rev-list --count --first-parent HEAD)
      revision=$((revision + revision_offset))
      
      echo "export PROJECT_REVISION_HASH_SHORT=$(git rev-parse --short HEAD)"
      echo "export PROJECT_REVISION_HASH=$(git rev-parse HEAD)"
      echo "export PROJECT_BRANCH=$(git symbolic-ref --short HEAD 2>/dev/null || echo 'detached')"
      echo "export PROJECT_REVISION=$revision"
      echo "export PROJECT_REVISION_OFFSET=$revision_offset"
      echo "export PROJECT_SVN_URL='$svn_url'"
      echo "export PROJECT_SVN_REVISION='$svn_revision'"
      echo "export PROJECT_SVN_BRANCH='$svn_branch'"
      echo "export PROJECT_VCS_TYPE='git-svn'"
      
      popd >/dev/null
      ;;
      
    git)
      # Export standard git variables
      pushd "$repo_path" >/dev/null
      
      # Calculate revision with offset
      local revision
      revision=$(git rev-list --count --first-parent HEAD)
      revision=$((revision + revision_offset))
      
      echo "export PROJECT_REVISION_HASH_SHORT=$(git rev-parse --short HEAD)"
      echo "export PROJECT_REVISION_HASH=$(git rev-parse HEAD)"
      echo "export PROJECT_BRANCH=$(git symbolic-ref --short HEAD 2>/dev/null || echo 'detached')"
      echo "export PROJECT_REVISION=$revision"
      echo "export PROJECT_REVISION_OFFSET=$revision_offset"
      echo "export PROJECT_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo 'none')"
      echo "export PROJECT_VCS_TYPE='git'"
      
      popd >/dev/null
      ;;
      
    *)
      echo "# Error: Unknown VCS type" >&2
      return 1
      ;;
  esac
}

#------------------------------------------------------------------------------
# Helper Functions
#------------------------------------------------------------------------------

# Print version info in human-readable format
print_version_info() {
  local repo_path="${1:-.}"
  local json_output
  
  json_output=$(get_version_info "$repo_path")
  
  # Parse JSON and print
  echo "Version Information:"
  echo "==================="
  echo "$json_output" | grep -o '"[^"]*": "[^"]*"' | while IFS=: read -r key value; do
    key=$(echo "$key" | tr -d '"')
    value=$(echo "$value" | tr -d '" ')
    printf "%-20s: %s\n" "$key" "$value"
  done
}

# Log library load (only if VH_DEBUG is set)
if [[ "${VH_DEBUG:-0}" == "1" ]]; then
  echo "version-helpers.sh v${VH_VERSION} loaded" >&2
fi
