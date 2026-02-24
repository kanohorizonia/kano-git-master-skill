#!/usr/bin/env bash
#
# bump-version.sh - Bump Git Master Skill version
#
# Usage:
#   ./bump-version.sh <major|minor|patch> [options]
#
# Arguments:
#   major         Bump major version (1.0.0 -> 2.0.0)
#   minor         Bump minor version (0.1.0 -> 0.2.0)
#   patch         Bump patch version (0.1.0 -> 0.1.1)
#
# Options:
#   --pre-release <label>  Set pre-release label (e.g., alpha, beta, rc.1)
#   --remove-pre-release   Remove pre-release label
#   --create-tag           Create git tag after version bump
#   --dry-run              Show what would be done without making changes
#   --help                 Show this help message
#
# Examples:
#   # Bump patch version (0.1.0-beta -> 0.1.1-beta)
#   ./bump-version.sh patch
#
#   # Bump minor version and remove pre-release (0.1.0-beta -> 0.2.0)
#   ./bump-version.sh minor --remove-pre-release
#
#   # Bump major version and create tag (0.1.0 -> 1.0.0)
#   ./bump-version.sh major --create-tag
#
#   # Set pre-release label (0.1.0 -> 0.1.0-rc.1)
#   ./bump-version.sh patch --pre-release rc.1
#
#   # Preview changes
#   ./bump-version.sh minor --dry-run
#

set -euo pipefail

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
VERSION_FILE="$PROJECT_ROOT/VERSION"

# Default options
BUMP_TYPE=""
PRE_RELEASE=""
REMOVE_PRE_RELEASE=0
CREATE_TAG=0
DRY_RUN=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") <major|minor|patch> [options]

Bump Git Master Skill version number in VERSION file.

Arguments:
  major         Bump major version (1.0.0 -> 2.0.0)
  minor         Bump minor version (0.1.0 -> 0.2.0)
  patch         Bump patch version (0.1.0 -> 0.1.1)

Options:
  --pre-release <label>  Set pre-release label (e.g., alpha, beta, rc.1)
  --remove-pre-release   Remove pre-release label
  --create-tag           Create git tag after version bump
  --dry-run              Show what would be done without making changes
  --help                 Show this help message

Examples:
  # Bump patch version (0.1.0-beta -> 0.1.1-beta)
  ./bump-version.sh patch

  # Bump minor version and remove pre-release (0.1.0-beta -> 0.2.0)
  ./bump-version.sh minor --remove-pre-release

  # Bump major version and create tag (0.1.0 -> 1.0.0)
  ./bump-version.sh major --create-tag

  # Set pre-release label (0.1.0 -> 0.1.0-rc.1)
  ./bump-version.sh patch --pre-release rc.1

  # Preview changes
  ./bump-version.sh minor --dry-run

Version Format:
  MAJOR.MINOR.PATCH[-PRE_RELEASE]
  
  Examples:
    0.1.0
    0.1.0-beta
    1.0.0-rc.1
    2.3.4-alpha
EOF
}

log_info() {
  echo -e "${BLUE}[ℹ]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[✓]${NC} $*"
}

log_error() {
  echo -e "${RED}[✗]${NC} $*" >&2
}

log_warning() {
  echo -e "${YELLOW}[!]${NC} $*"
}

# Parse semantic version
parse_version() {
  local version="$1"
  local major minor patch pre_release
  
  # Remove any whitespace
  version=$(echo "$version" | tr -d '[:space:]')
  
  # Split version and pre-release
  if [[ "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-(.+))?$ ]]; then
    major="${BASH_REMATCH[1]}"
    minor="${BASH_REMATCH[2]}"
    patch="${BASH_REMATCH[3]}"
    pre_release="${BASH_REMATCH[5]:-}"
    
    echo "$major|$minor|$patch|$pre_release"
  else
    log_error "Invalid version format: $version"
    log_error "Expected format: MAJOR.MINOR.PATCH[-PRE_RELEASE]"
    return 1
  fi
}

# Bump version number
bump_version() {
  local current_version="$1"
  local bump_type="$2"
  local new_pre_release="$3"
  local remove_pre="$4"
  
  local version_parts
  version_parts=$(parse_version "$current_version")
  
  IFS='|' read -r major minor patch current_pre <<< "$version_parts"
  
  # Bump version based on type
  case "$bump_type" in
    major)
      major=$((major + 1))
      minor=0
      patch=0
      ;;
    minor)
      minor=$((minor + 1))
      patch=0
      ;;
    patch)
      patch=$((patch + 1))
      ;;
    *)
      log_error "Invalid bump type: $bump_type"
      return 1
      ;;
  esac
  
  # Construct new version
  local new_version="$major.$minor.$patch"
  
  # Handle pre-release
  if [[ "$remove_pre" -eq 1 ]]; then
    # Remove pre-release label
    :
  elif [[ -n "$new_pre_release" ]]; then
    # Set new pre-release label
    new_version="$new_version-$new_pre_release"
  elif [[ -n "$current_pre" ]]; then
    # Keep existing pre-release label
    new_version="$new_version-$current_pre"
  fi
  
  echo "$new_version"
}

# Update VERSION file
update_version_file() {
  local new_version="$1"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN: Would write to $VERSION_FILE"
    return 0
  fi
  
  echo "$new_version" > "$VERSION_FILE"
  log_success "Updated VERSION file: $new_version"
}

# Create git tag
create_git_tag() {
  local version="$1"
  local tag_name="v$version"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN: Would create tag: $tag_name"
    return 0
  fi
  
  # Check if tag already exists
  if git -C "$PROJECT_ROOT" rev-parse "$tag_name" &>/dev/null; then
    log_error "Tag already exists: $tag_name"
    return 1
  fi
  
  # Create annotated tag
  git -C "$PROJECT_ROOT" tag -a "$tag_name" -m "Release version $version"
  log_success "Created git tag: $tag_name"
  
  log_info "To push tag to remote:"
  log_info "  git push origin $tag_name"
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
      --pre-release)
        PRE_RELEASE="${2:-}"
        if [[ -z "$PRE_RELEASE" ]]; then
          log_error "--pre-release requires a label"
          exit 1
        fi
        shift 2
        ;;
      --remove-pre-release)
        REMOVE_PRE_RELEASE=1
        shift
        ;;
      --create-tag)
        CREATE_TAG=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      -*)
        log_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        positional_args+=("$1")
        shift
        ;;
    esac
  done
  
  # Get bump type
  if [[ ${#positional_args[@]} -eq 0 ]]; then
    log_error "Missing bump type (major, minor, or patch)"
    usage
    exit 1
  fi
  
  BUMP_TYPE="${positional_args[0]}"
  
  # Validate bump type
  if [[ ! "$BUMP_TYPE" =~ ^(major|minor|patch)$ ]]; then
    log_error "Invalid bump type: $BUMP_TYPE"
    log_error "Must be one of: major, minor, patch"
    exit 1
  fi
  
  # Check if VERSION file exists
  if [[ ! -f "$VERSION_FILE" ]]; then
    log_error "VERSION file not found: $VERSION_FILE"
    exit 1
  fi
  
  # Read current version
  local current_version
  current_version=$(cat "$VERSION_FILE" | tr -d '[:space:]')
  
  log_info "Current version: $current_version"
  
  # Calculate new version
  local new_version
  new_version=$(bump_version "$current_version" "$BUMP_TYPE" "$PRE_RELEASE" "$REMOVE_PRE_RELEASE")
  
  log_info "New version: $new_version"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN MODE - No changes will be made"
    echo ""
  fi
  
  # Update VERSION file
  update_version_file "$new_version"
  
  # Create git tag if requested
  if [[ "$CREATE_TAG" -eq 1 ]]; then
    create_git_tag "$new_version"
  fi
  
  echo ""
  log_success "Version bump complete!"
  
  if [[ "$DRY_RUN" -eq 0 ]]; then
    echo ""
    log_info "Next steps:"
    log_info "  1. Review changes: git diff VERSION"
    log_info "  2. Commit changes: git add VERSION && git commit -m 'chore: bump version to $new_version'"
    if [[ "$CREATE_TAG" -eq 0 ]]; then
      log_info "  3. Create tag: ./scripts/internal/create-tag.sh v$new_version"
    else
      log_info "  3. Push tag: git push origin v$new_version"
    fi
  fi
}

# Run main
main "$@"
