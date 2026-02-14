#!/usr/bin/env bash
#
# create-tag.sh - Create git tag for Git Master Skill
#
# Usage:
#   ./create-tag.sh [tag-name] [options]
#
# Arguments:
#   tag-name      Tag name (e.g., v1.0.0). If omitted, uses VERSION file
#
# Options:
#   --from-version-file    Create tag from VERSION file (default if no tag-name)
#   -m, --message <msg>    Tag message (creates annotated tag)
#   --lightweight          Create lightweight tag (default if no message)
#   --force                Force create tag (overwrite if exists)
#   --push                 Push tag to remote after creation
#   --dry-run              Show what would be done without making changes
#   --help                 Show this help message
#
# Examples:
#   # Create tag from VERSION file
#   ./create-tag.sh --from-version-file
#
#   # Create annotated tag
#   ./create-tag.sh v1.0.0 -m "Release version 1.0.0"
#
#   # Create lightweight tag
#   ./create-tag.sh v1.0.0 --lightweight
#
#   # Create and push tag
#   ./create-tag.sh v1.0.0 -m "Release 1.0.0" --push
#
#   # Force overwrite existing tag
#   ./create-tag.sh v1.0.0 --force
#

set -euo pipefail

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"
VERSION_FILE="$PROJECT_ROOT/VERSION"

# Default options
TAG_NAME=""
TAG_MESSAGE=""
FROM_VERSION_FILE=0
LIGHTWEIGHT=0
FORCE=0
PUSH=0
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
Usage: $(basename "$0") [tag-name] [options]

Create git tag for Git Master Skill.

Arguments:
  tag-name      Tag name (e.g., v1.0.0). If omitted, uses VERSION file

Options:
  --from-version-file    Create tag from VERSION file (default if no tag-name)
  -m, --message <msg>    Tag message (creates annotated tag)
  --lightweight          Create lightweight tag (default if no message)
  --force                Force create tag (overwrite if exists)
  --push                 Push tag to remote after creation
  --dry-run              Show what would be done without making changes
  --help                 Show this help message

Tag Types:
  Annotated    Recommended for releases (includes tagger, date, message)
  Lightweight  Simple pointer to a commit (no metadata)

Examples:
  # Create tag from VERSION file (annotated)
  ./create-tag.sh --from-version-file -m "Beta release"

  # Create annotated tag
  ./create-tag.sh v1.0.0 -m "Release version 1.0.0"

  # Create lightweight tag
  ./create-tag.sh v1.0.0 --lightweight

  # Create and push tag
  ./create-tag.sh v1.0.0 -m "Release 1.0.0" --push

  # Force overwrite existing tag
  ./create-tag.sh v1.0.0 --force -m "Updated release"

  # Preview without creating
  ./create-tag.sh v1.0.0 --dry-run
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

# Get version from VERSION file
get_version_from_file() {
  if [[ ! -f "$VERSION_FILE" ]]; then
    log_error "VERSION file not found: $VERSION_FILE"
    return 1
  fi
  
  local version
  version=$(cat "$VERSION_FILE" | tr -d '[:space:]')
  
  if [[ -z "$version" ]]; then
    log_error "VERSION file is empty"
    return 1
  fi
  
  # Add 'v' prefix if not present
  if [[ ! "$version" =~ ^v ]]; then
    version="v$version"
  fi
  
  echo "$version"
}

# Check if tag exists
tag_exists() {
  local tag="$1"
  git -C "$PROJECT_ROOT" rev-parse "$tag" &>/dev/null
}

# Create git tag
create_tag() {
  local tag="$1"
  local message="$2"
  local is_lightweight="$3"
  local force="$4"
  
  # Check if tag exists
  if tag_exists "$tag"; then
    if [[ "$force" -eq 0 ]]; then
      log_error "Tag already exists: $tag"
      log_error "Use --force to overwrite"
      return 1
    else
      log_warning "Tag exists, will be overwritten: $tag"
    fi
  fi
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN: Would create tag: $tag"
    if [[ "$is_lightweight" -eq 1 ]]; then
      log_info "Type: Lightweight"
    else
      log_info "Type: Annotated"
      log_info "Message: $message"
    fi
    return 0
  fi
  
  # Create tag
  local force_flag=""
  if [[ "$force" -eq 1 ]]; then
    force_flag="-f"
  fi
  
  if [[ "$is_lightweight" -eq 1 ]]; then
    # Lightweight tag
    git -C "$PROJECT_ROOT" tag $force_flag "$tag"
    log_success "Created lightweight tag: $tag"
  else
    # Annotated tag
    git -C "$PROJECT_ROOT" tag $force_flag -a "$tag" -m "$message"
    log_success "Created annotated tag: $tag"
  fi
}

# Push tag to remote
push_tag() {
  local tag="$1"
  local force="$2"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN: Would push tag to remote: $tag"
    return 0
  fi
  
  local force_flag=""
  if [[ "$force" -eq 1 ]]; then
    force_flag="--force"
  fi
  
  git -C "$PROJECT_ROOT" push $force_flag origin "$tag"
  log_success "Pushed tag to remote: $tag"
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
      --from-version-file)
        FROM_VERSION_FILE=1
        shift
        ;;
      -m|--message)
        TAG_MESSAGE="${2:-}"
        if [[ -z "$TAG_MESSAGE" ]]; then
          log_error "--message requires a value"
          exit 1
        fi
        shift 2
        ;;
      --lightweight)
        LIGHTWEIGHT=1
        shift
        ;;
      --force)
        FORCE=1
        shift
        ;;
      --push)
        PUSH=1
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
  
  # Determine tag name
  if [[ ${#positional_args[@]} -gt 0 ]]; then
    TAG_NAME="${positional_args[0]}"
  elif [[ "$FROM_VERSION_FILE" -eq 1 ]] || [[ ${#positional_args[@]} -eq 0 ]]; then
    # Use VERSION file if no tag name provided
    TAG_NAME=$(get_version_from_file)
    FROM_VERSION_FILE=1
  fi
  
  if [[ -z "$TAG_NAME" ]]; then
    log_error "No tag name specified"
    usage
    exit 1
  fi
  
  # Validate tag name format (should start with 'v')
  if [[ ! "$TAG_NAME" =~ ^v[0-9] ]]; then
    log_warning "Tag name should start with 'v' (e.g., v1.0.0)"
    log_warning "Current: $TAG_NAME"
  fi
  
  # Determine tag type
  if [[ -n "$TAG_MESSAGE" ]]; then
    # Annotated tag (has message)
    LIGHTWEIGHT=0
  elif [[ "$LIGHTWEIGHT" -eq 0 ]] && [[ -z "$TAG_MESSAGE" ]]; then
    # No message and not explicitly lightweight - default to annotated with auto message
    if [[ "$FROM_VERSION_FILE" -eq 1 ]]; then
      local version="${TAG_NAME#v}"
      TAG_MESSAGE="Release version $version"
    else
      TAG_MESSAGE="Release $TAG_NAME"
    fi
  fi
  
  log_info "Tag name: $TAG_NAME"
  
  if [[ "$FROM_VERSION_FILE" -eq 1 ]]; then
    log_info "Source: VERSION file"
  fi
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    log_warning "DRY RUN MODE - No changes will be made"
    echo ""
  fi
  
  # Create tag
  create_tag "$TAG_NAME" "$TAG_MESSAGE" "$LIGHTWEIGHT" "$FORCE"
  
  # Push tag if requested
  if [[ "$PUSH" -eq 1 ]]; then
    push_tag "$TAG_NAME" "$FORCE"
  fi
  
  echo ""
  log_success "Tag creation complete!"
  
  if [[ "$DRY_RUN" -eq 0 ]]; then
    echo ""
    log_info "Tag details:"
    if [[ "$LIGHTWEIGHT" -eq 0 ]]; then
      git -C "$PROJECT_ROOT" show "$TAG_NAME" --no-patch
    else
      git -C "$PROJECT_ROOT" log -1 --oneline "$TAG_NAME"
    fi
    
    if [[ "$PUSH" -eq 0 ]]; then
      echo ""
      log_info "To push tag to remote:"
      log_info "  git push origin $TAG_NAME"
    fi
  fi
}

# Run main
main "$@"
