#!/usr/bin/env bash
#
# init-empty-repo.sh - Initialize an empty remote repository
#
# Purpose:
#   Quickly initialize an empty remote repository with a single commit
#   All parameters are optional with sensible defaults
#   Includes safety checks to prevent accidental overwrites
#
# Usage:
#   ./init-empty-repo.sh <remote-url> [options]
#
# Arguments:
#   remote-url        Remote repository URL (required)
#
# Options:
#   --branch <name>                Branch name (default: main)
#   --message <text>               Commit message (default: "Initial commit")
#   --file <name>                  First file name (default: README.md)
#   --content <text>               File content (default: "# Repository\n\nInitialized.")
#   --dir <path>                   Local directory (default: temp directory)
#   --keep-local                   Keep local directory after push
#   --force-overwrite-remote       Force overwrite remote (DANGEROUS: destroys existing content)
#   -h, --help                     Show help
#
# Safety Features:
#   - Pre-checks if remote already has content
#   - Refuses to push if remote is not empty (unless forced)
#   - Verbose flag name (--force-overwrite-remote) to prevent accidents
#   - 3-second warning delay before destructive operations
#   - Rejects old --force flag with helpful error message
#
# Examples:
#   # Minimal - just URL (safe, will fail if remote has content)
#   ./init-empty-repo.sh git@github.com:user/repo.git
#
#   # Custom branch
#   ./init-empty-repo.sh git@github.com:user/repo.git --branch develop
#
#   # Custom commit message
#   ./init-empty-repo.sh git@github.com:user/repo.git --message "feat: Initial setup"
#
#   # Custom file
#   ./init-empty-repo.sh git@github.com:user/repo.git --file index.html --content "<h1>Hello</h1>"
#
#   # Keep local copy
#   ./init-empty-repo.sh git@github.com:user/repo.git --dir ~/my-repo --keep-local
#
#   # Force overwrite (DANGEROUS - destroys existing content!)
#   ./init-empty-repo.sh git@github.com:user/repo.git --force-overwrite-remote
#

set -euo pipefail

# Default configuration
REMOTE_URL=""
BRANCH_NAME="main"
COMMIT_MESSAGE="Initial commit"
FILE_NAME="README.md"
FILE_CONTENT="# Repository

Initialized.
"
LOCAL_DIR=""
KEEP_LOCAL=0
FORCE_OVERWRITE=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") <remote-url> [options]

Initialize an empty remote repository with a single commit.
All options have sensible defaults.

Arguments:
  remote-url        Remote repository URL (required)

Options:
  --branch <name>                Branch name (default: main)
  --message <text>               Commit message (default: "Initial commit")
  --file <name>                  First file name (default: README.md)
  --content <text>               File content (default: "# Repository\\n\\nInitialized.")
  --dir <path>                   Local directory (default: temp directory)
  --keep-local                   Keep local directory after push
  --force-overwrite-remote       Force overwrite remote (DANGEROUS: destroys existing content)
  -h, --help                     Show help

Examples:
  # Minimal usage
  ./init-empty-repo.sh git@github.com:user/repo.git

  # Custom branch
  ./init-empty-repo.sh git@github.com:user/repo.git --branch develop

  # Custom commit message
  ./init-empty-repo.sh git@github.com:user/repo.git --message "feat: Initial setup"

  # Custom file and content
  ./init-empty-repo.sh git@github.com:user/repo.git \\
    --file index.html \\
    --content "<h1>Hello World</h1>"

  # Multiple files (create file, then add more manually)
  ./init-empty-repo.sh git@github.com:user/repo.git \\
    --dir ~/my-repo \\
    --keep-local

  # Force overwrite (WARNING: DESTROYS existing remote content!)
  ./init-empty-repo.sh git@github.com:user/repo.git --force-overwrite-remote

Safety:
  - Script checks if remote already has content before pushing
  - If remote is not empty, script will fail unless --force-overwrite-remote is used
  - The flag name is intentionally verbose to prevent accidental overwrites

AI-Friendly:
  All parameters are optional except the URL.
  Defaults are sensible for quick initialization.
  Use --keep-local to inspect or modify before cleanup.
EOF
}

log_info() {
  echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[✓]${NC} $*"
}

log_error() {
  echo -e "${RED}[✗]${NC} $*" >&2
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
      --branch)
        BRANCH_NAME="${2:-}"
        shift 2
        ;;
      --message)
        COMMIT_MESSAGE="${2:-}"
        shift 2
        ;;
      --file)
        FILE_NAME="${2:-}"
        shift 2
        ;;
      --content)
        FILE_CONTENT="${2:-}"
        shift 2
        ;;
      --dir)
        LOCAL_DIR="${2:-}"
        shift 2
        ;;
      --keep-local)
        KEEP_LOCAL=1
        shift
        ;;
      --force-overwrite-remote)
        FORCE_OVERWRITE=1
        shift
        ;;
      --force)
        log_error "The --force flag has been replaced with --force-overwrite-remote"
        log_error "This is intentional to prevent accidental data loss"
        log_error "Use: --force-overwrite-remote (if you really want to overwrite)"
        exit 1
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
  
  # Get remote URL
  if [[ ${#positional_args[@]} -lt 1 ]]; then
    log_error "Remote URL is required"
    usage
    exit 1
  fi
  
  REMOTE_URL="${positional_args[0]}"
  
  # Set default local directory if not specified
  if [[ -z "$LOCAL_DIR" ]]; then
    LOCAL_DIR="$(mktemp -d)"
  fi
  
  # Show configuration
  log_info "Initializing repository..."
  log_info "  Remote: $REMOTE_URL"
  log_info "  Branch: $BRANCH_NAME"
  log_info "  File: $FILE_NAME"
  log_info "  Local: $LOCAL_DIR"
  echo ""
  
  # Pre-test: Check if remote already has content
  log_info "Checking if remote repository is empty..."
  if git ls-remote "$REMOTE_URL" HEAD &>/dev/null; then
    local remote_refs
    remote_refs=$(git ls-remote "$REMOTE_URL" 2>/dev/null | wc -l)
    
    if [[ "$remote_refs" -gt 0 ]]; then
      log_error "Remote repository is NOT empty!"
      echo ""
      echo "Remote has $remote_refs reference(s):"
      git ls-remote "$REMOTE_URL" 2>/dev/null | head -5
      if [[ "$remote_refs" -gt 5 ]]; then
        echo "... and $((remote_refs - 5)) more"
      fi
      echo ""
      
      if [[ "$FORCE_OVERWRITE" -eq 0 ]]; then
        log_error "Refusing to overwrite existing content"
        log_error ""
        log_error "If you really want to overwrite the remote repository, use:"
        log_error "  --force-overwrite-remote"
        log_error ""
        log_error "WARNING: This will DESTROY all existing content in the remote!"
        exit 1
      else
        echo ""
        log_info "⚠️  WARNING: --force-overwrite-remote flag detected"
        log_info "⚠️  This will DESTROY all existing content in the remote!"
        log_info "⚠️  Proceeding in 3 seconds... (Ctrl+C to cancel)"
        sleep 3
        echo ""
      fi
    else
      log_info "✓ Remote repository is empty (safe to initialize)"
    fi
  else
    log_info "✓ Remote repository does not exist yet or is empty"
  fi
  echo ""
  
  # Create local directory
  mkdir -p "$LOCAL_DIR"
  cd "$LOCAL_DIR"
  
  # Initialize git
  log_info "Creating local repository..."
  git init -q
  git checkout -b "$BRANCH_NAME" 2>/dev/null || git checkout "$BRANCH_NAME"
  
  # Create file
  log_info "Creating $FILE_NAME..."
  echo -e "$FILE_CONTENT" > "$FILE_NAME"
  
  # Commit
  log_info "Creating commit..."
  git add "$FILE_NAME"
  git commit -q -m "$COMMIT_MESSAGE"
  
  # Add remote
  log_info "Adding remote..."
  git remote add origin "$REMOTE_URL"
  
  # Push
  log_info "Pushing to remote..."
  if [[ "$FORCE_OVERWRITE" -eq 1 ]]; then
    git push -q -f origin "$BRANCH_NAME" 2>&1 || {
      log_error "Push failed"
      exit 1
    }
  else
    git push -q -u origin "$BRANCH_NAME" 2>&1 || {
      log_error "Push failed"
      log_error "If the remote already has content, use --force-overwrite-remote"
      exit 1
    }
  fi
  
  log_success "Repository initialized successfully!"
  echo ""
  echo "Repository: $REMOTE_URL"
  echo "Branch: $BRANCH_NAME"
  echo "Commit: $(git rev-parse --short HEAD) - $COMMIT_MESSAGE"
  echo ""
  
  if [[ "$KEEP_LOCAL" -eq 1 ]]; then
    log_info "Local directory preserved: $LOCAL_DIR"
    echo ""
    echo "To continue working:"
    echo "  cd $LOCAL_DIR"
  else
    log_info "Cleaning up local directory..."
    cd /
    rm -rf "$LOCAL_DIR"
  fi
  
  echo ""
  echo "To clone:"
  echo "  git clone $REMOTE_URL"
}

# Run main
main "$@"
