#!/usr/bin/env bash
#
# init-repo-workflow.sh - Orchestrate complete repository initialization workflow
#
# Purpose:
#   Execute the complete repository initialization workflow including:
#   - Remote repository detection
#   - Main branch initialization
#   - Multi-remote setup
#   - Orphan branch creation
#   - Submodule addition
#
# Usage:
#   ./init-repo-workflow.sh --repo-url <url> [options]
#
# Required Options:
#   --repo-url <url>        Repository URL (SSH or HTTP)
#
# Optional Options:
#   --repo-http-url <url>   HTTP URL (enables multi-remote mode)
#   --upstream-ssh <url>    Upstream SSH URL
#   --upstream-http <url>   Upstream HTTP URL
#   --repo-dir <path>       Local repository directory
#   --orphan-branch <name>  Orphan branch name (default: dev/gitmaster)
#   --submodule <url:path>  Submodule to add (can be repeated)
#   --skip-main-init        Skip main branch initialization
#   --skip-orphan           Skip orphan branch creation
#   --skip-submodules       Skip submodule addition
#   --dry-run               Show what would be done
#   -h, --help              Show help
#
# Examples:
#   # Minimal - just initialize repository
#   ./init-repo-workflow.sh --repo-url git@github.com:user/repo.git
#
#   # Full workflow with multi-remote and orphan branch
#   ./init-repo-workflow.sh \
#     --repo-url git@github.com:user/repo.git \
#     --repo-http-url https://github.com/user/repo.git \
#     --orphan-branch dev/tools
#
#   # With upstream and submodules
#   ./init-repo-workflow.sh \
#     --repo-url git@github.com:user/repo.git \
#     --repo-http-url https://github.com/user/repo.git \
#     --upstream-ssh git@github.com:original/repo.git \
#     --upstream-http https://github.com/original/repo.git \
#     --submodule "git@github.com:user/skill1.git:skills/skill1" \
#     --submodule "git@github.com:user/skill2.git:skills/skill2"
#
#   # Dry run to preview workflow
#   ./init-repo-workflow.sh \
#     --repo-url git@github.com:user/repo.git \
#     --dry-run
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

# Source git-helpers
if [[ -f "$LIB_DIR/git-helpers.sh" ]]; then
  source "$LIB_DIR/git-helpers.sh"
else
  echo "ERROR: Cannot find git-helpers.sh at $LIB_DIR/git-helpers.sh" >&2
  exit 1
fi

# Default configuration
REPO_URL=""
REPO_HTTP_URL=""
UPSTREAM_SSH=""
UPSTREAM_HTTP=""
REPO_DIR=""
ORPHAN_BRANCH="dev/gitmaster"
SUBMODULES=()
SKIP_MAIN_INIT=0
SKIP_ORPHAN=0
SKIP_SUBMODULES=0
DRY_RUN=0

# Workflow state tracking
REMOTE_STATUS=""
STEPS_COMPLETED=()
STEPS_SKIPPED=()
RESOURCES_CREATED=()
ORIGINAL_BRANCH=""
STASH_REF=""

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") --repo-url <url> [options]

Orchestrate complete repository initialization workflow.

Required Options:
  --repo-url <url>        Repository URL (SSH or HTTP)

Optional Options:
  --repo-http-url <url>   HTTP URL (enables multi-remote mode)
  --upstream-ssh <url>    Upstream SSH URL
  --upstream-http <url>   Upstream HTTP URL
  --repo-dir <path>       Local repository directory
  --orphan-branch <name>  Orphan branch name (default: dev/gitmaster)
  --submodule <url:path>  Submodule to add (can be repeated)
  --skip-main-init        Skip main branch initialization
  --skip-orphan           Skip orphan branch creation
  --skip-submodules       Skip submodule addition
  --dry-run               Show what would be done
  -h, --help              Show help

Workflow Steps:
  1. Remote detection - Check if remote is empty or has content
  2. Main branch initialization - Initialize or pull main branch
  3. Multi-remote setup - Configure SSH/HTTP fallback (if HTTP URL provided)
  4. Orphan branch creation - Create isolated branch for tools
  5. Submodule addition - Add specified submodules

Examples:
  # Minimal - just initialize repository
  ./init-repo-workflow.sh --repo-url git@github.com:user/repo.git

  # Full workflow with multi-remote and orphan branch
  ./init-repo-workflow.sh \\
    --repo-url git@github.com:user/repo.git \\
    --repo-http-url https://github.com/user/repo.git \\
    --orphan-branch dev/tools

  # With upstream and submodules
  ./init-repo-workflow.sh \\
    --repo-url git@github.com:user/repo.git \\
    --repo-http-url https://github.com/user/repo.git \\
    --upstream-ssh git@github.com:original/repo.git \\
    --upstream-http https://github.com/original/repo.git \\
    --submodule "git@github.com:user/skill1.git:skills/skill1" \\
    --submodule "git@github.com:user/skill2.git:skills/skill2"

  # Dry run to preview workflow
  ./init-repo-workflow.sh \\
    --repo-url git@github.com:user/repo.git \\
    --dry-run

Notes:
  - Workflow stops on first failure
  - Steps are skipped if already completed or explicitly skipped
  - Summary report shows all actions taken
EOF
}

# Validate required arguments
validate_arguments() {
  if [[ -z "$REPO_URL" ]]; then
    gith_error "Repository URL is required (--repo-url)"
    usage
    exit 1
  fi

  # Validate URL format
  if ! gith_validate_url "$REPO_URL"; then
    gith_error "Invalid repository URL format: $REPO_URL"
    exit 1
  fi

  # Validate HTTP URL if provided
  if [[ -n "$REPO_HTTP_URL" ]] && ! gith_validate_url "$REPO_HTTP_URL"; then
    gith_error "Invalid HTTP URL format: $REPO_HTTP_URL"
    exit 1
  fi

  # Validate upstream URLs if provided
  if [[ -n "$UPSTREAM_SSH" ]] && ! gith_validate_url "$UPSTREAM_SSH"; then
    gith_error "Invalid upstream SSH URL format: $UPSTREAM_SSH"
    exit 1
  fi

  if [[ -n "$UPSTREAM_HTTP" ]] && ! gith_validate_url "$UPSTREAM_HTTP"; then
    gith_error "Invalid upstream HTTP URL format: $UPSTREAM_HTTP"
    exit 1
  fi

  # Validate orphan branch name
  if ! gith_validate_branch_name "$ORPHAN_BRANCH"; then
    gith_error "Invalid orphan branch name: $ORPHAN_BRANCH"
    exit 1
  fi

  return 0
}

# Step 1: Detect remote status
detect_remote_status() {
  gith_log "INFO" "=== Step 1: Remote Detection ==="

  if [[ "$DRY_RUN" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would check remote status: $REPO_URL"
    REMOTE_STATUS="unknown"
    return 0
  fi

  gith_log "INFO" "Checking remote status: $REPO_URL"

  # Check if remote is empty
  if gith_is_remote_empty "$REPO_URL"; then
    REMOTE_STATUS="empty"
    gith_log "INFO" "Remote repository is empty"
  else
    local exit_code=$?
    if [[ $exit_code -eq 2 ]]; then
      REMOTE_STATUS="not-accessible"
      gith_error "Remote repository is not accessible"
      gith_error "Please check:"
      gith_error "  - Network connectivity"
      gith_error "  - Repository URL: $REPO_URL"
      gith_error "  - Authentication credentials"
      return 1
    else
      REMOTE_STATUS="has-content"
      gith_log "INFO" "Remote repository has existing content"

      # List existing references
      gith_log "INFO" "Existing references:"
      git ls-remote "$REPO_URL" 2>/dev/null | head -5 | while read -r line; do
        gith_log "INFO" "  $line"
      done
    fi
  fi

  STEPS_COMPLETED+=("remote-detection")
  return 0
}

# Step 2: Initialize main branch
initialize_main_branch() {
  gith_log "INFO" "=== Step 2: Main Branch Initialization ==="

  # Check if step should be skipped
  if [[ "$SKIP_MAIN_INIT" == "1" ]]; then
    gith_log "INFO" "Skipping main branch initialization (--skip-main-init flag)"
    STEPS_SKIPPED+=("main-init: explicitly skipped")
    return 0
  fi

  # Check if repository directory already exists
  if [[ -n "$REPO_DIR" ]] && [[ -d "$REPO_DIR" ]] && gith_is_git_repo "$REPO_DIR"; then
    gith_log "INFO" "Repository already exists at: $REPO_DIR"
    STEPS_SKIPPED+=("main-init: repository already exists")
    return 0
  fi

  # Initialize based on remote status
  if [[ "$REMOTE_STATUS" == "empty" ]]; then
    gith_log "INFO" "Initializing empty remote repository"

    if [[ "$DRY_RUN" == "1" ]]; then
      gith_log "INFO" "[DRY-RUN] Would initialize empty repository: $REPO_URL"
      return 0
    fi

    # Build init-empty-repo.sh command
    local init_cmd=("$SCRIPT_DIR/init-empty-repo.sh" "$REPO_URL")

    if [[ -n "$REPO_DIR" ]]; then
      init_cmd+=("--dir" "$REPO_DIR" "--keep-local")
    fi

    # Execute init-empty-repo.sh
    if ! "${init_cmd[@]}"; then
      gith_error "Failed to initialize empty repository"
      return 1
    fi

    RESOURCES_CREATED+=("main branch with initial commit")

  elif [[ "$REMOTE_STATUS" == "has-content" ]]; then
    gith_log "INFO" "Cloning existing repository content"

    if [[ "$DRY_RUN" == "1" ]]; then
      gith_log "INFO" "[DRY-RUN] Would clone repository: $REPO_URL"
      return 0
    fi

    # Determine clone directory
    local clone_dir="$REPO_DIR"
    if [[ -z "$clone_dir" ]]; then
      # Extract repo name from URL
      clone_dir=$(basename "$REPO_URL" .git)
    fi

    # Clone repository
    if ! git clone "$REPO_URL" "$clone_dir"; then
      gith_error "Failed to clone repository"
      return 1
    fi

    REPO_DIR="$clone_dir"
    RESOURCES_CREATED+=("cloned repository to $REPO_DIR")

  else
    gith_error "Cannot initialize main branch: remote status is $REMOTE_STATUS"
    return 1
  fi

  STEPS_COMPLETED+=("main-init")
  return 0
}

# Step 3: Setup multi-remote configuration
setup_multi_remote() {
  gith_log "INFO" "=== Step 3: Multi-Remote Setup ==="

  # Check if HTTP URL provided (required for multi-remote mode)
  if [[ -z "$REPO_HTTP_URL" ]]; then
    gith_log "INFO" "Skipping multi-remote setup (no HTTP URL provided)"
    STEPS_SKIPPED+=("multi-remote: no HTTP URL provided")
    return 0
  fi

  if [[ "$DRY_RUN" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would setup multi-remote configuration"
    return 0
  fi

  # Ensure we have a repository directory
  if [[ -z "$REPO_DIR" ]] || [[ ! -d "$REPO_DIR" ]]; then
    gith_error "Repository directory not found for multi-remote setup"
    return 1
  fi

  gith_log "INFO" "Configuring multi-remote setup"

  # Build setup-multi-remote.sh command
  local setup_cmd=("$SCRIPT_DIR/setup-multi-remote.sh")
  setup_cmd+=("--origin-ssh" "$REPO_URL")
  setup_cmd+=("--origin-http" "$REPO_HTTP_URL")

  if [[ -n "$UPSTREAM_SSH" ]]; then
    setup_cmd+=("--upstream-ssh" "$UPSTREAM_SSH")
  fi

  if [[ -n "$UPSTREAM_HTTP" ]]; then
    setup_cmd+=("--upstream-http" "$UPSTREAM_HTTP")
  fi

  setup_cmd+=("--dir" "$REPO_DIR")
  setup_cmd+=("--validate")

  # Execute setup-multi-remote.sh
  if ! "${setup_cmd[@]}"; then
    gith_error "Failed to setup multi-remote configuration"
    return 1
  fi

  RESOURCES_CREATED+=("multi-remote configuration")
  STEPS_COMPLETED+=("multi-remote-setup")
  return 0
}

# Step 4: Create orphan branch
create_orphan_branch() {
  gith_log "INFO" "=== Step 4: Orphan Branch Creation ==="

  # Check if step should be skipped
  if [[ "$SKIP_ORPHAN" == "1" ]]; then
    gith_log "INFO" "Skipping orphan branch creation (--skip-orphan flag)"
    STEPS_SKIPPED+=("orphan-creation: explicitly skipped")
    return 0
  fi

  if [[ "$DRY_RUN" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would create orphan branch: $ORPHAN_BRANCH"
    return 0
  fi

  # Ensure we have a repository directory
  if [[ -z "$REPO_DIR" ]] || [[ ! -d "$REPO_DIR" ]]; then
    gith_error "Repository directory not found for orphan branch creation"
    return 1
  fi

  gith_log "INFO" "Creating orphan branch: $ORPHAN_BRANCH"

  # Build create-orphan-branch.sh command
  local orphan_cmd=("$SCRIPT_DIR/create-orphan-branch.sh")
  orphan_cmd+=("--branch" "$ORPHAN_BRANCH")
  orphan_cmd+=("--dir" "$REPO_DIR")
  orphan_cmd+=("--message" "chore: Initialize development tools branch")
  orphan_cmd+=("--push")
  orphan_cmd+=("--return")

  # Execute create-orphan-branch.sh
  if ! "${orphan_cmd[@]}"; then
    gith_error "Failed to create orphan branch"
    return 1
  fi

  RESOURCES_CREATED+=("orphan branch: $ORPHAN_BRANCH")
  STEPS_COMPLETED+=("orphan-creation")
  return 0
}

# Step 5: Add submodules
add_submodules() {
  gith_log "INFO" "=== Step 5: Submodule Addition ==="

  # Check if step should be skipped
  if [[ "$SKIP_SUBMODULES" == "1" ]]; then
    gith_log "INFO" "Skipping submodule addition (--skip-submodules flag)"
    STEPS_SKIPPED+=("submodule-addition: explicitly skipped")
    return 0
  fi

  # Check if any submodules specified
  if [[ ${#SUBMODULES[@]} -eq 0 ]]; then
    gith_log "INFO" "Skipping submodule addition (no submodules specified)"
    STEPS_SKIPPED+=("submodule-addition: no submodules specified")
    return 0
  fi

  if [[ "$DRY_RUN" == "1" ]]; then
    gith_log "INFO" "[DRY-RUN] Would add ${#SUBMODULES[@]} submodule(s)"
    for submodule in "${SUBMODULES[@]}"; do
      gith_log "INFO" "[DRY-RUN]   - $submodule"
    done
    return 0
  fi

  # Ensure we have a repository directory
  if [[ -z "$REPO_DIR" ]] || [[ ! -d "$REPO_DIR" ]]; then
    gith_error "Repository directory not found for submodule addition"
    return 1
  fi

  gith_log "INFO" "Adding ${#SUBMODULES[@]} submodule(s)"

  local added_count=0
  local failed_count=0

  # Process each submodule
  for submodule in "${SUBMODULES[@]}"; do
    # Parse submodule specification: url:path
    local submodule_url="${submodule%%:*}"
    local submodule_path="${submodule##*:}"

    if [[ -z "$submodule_url" ]] || [[ -z "$submodule_path" ]]; then
      gith_error "Invalid submodule specification: $submodule"
      gith_error "Expected format: url:path"
      ((failed_count++))
      continue
    fi

    gith_log "INFO" "Adding submodule: $submodule_path"
    gith_log "INFO" "  URL: $submodule_url"

    # Build kog-submodule add command
    local submodule_cmd=("$SCRIPT_DIR/../submodules/kog-submodule.sh" "add")
    submodule_cmd+=("--path" "$submodule_path")

    # Determine if URL is SSH or HTTPS
    if [[ "$submodule_url" =~ ^git@ ]]; then
      submodule_cmd+=("--ssh" "$submodule_url")
    elif [[ "$submodule_url" =~ ^https?:// ]]; then
      submodule_cmd+=("--https" "$submodule_url")
    else
      gith_error "Unsupported submodule URL format: $submodule_url"
      ((failed_count++))
      continue
    fi

    # Execute kog-submodule add (from repository directory)
    if (cd "$REPO_DIR" && "${submodule_cmd[@]}"); then
      gith_log "INFO" "Successfully added submodule: $submodule_path"
      RESOURCES_CREATED+=("submodule: $submodule_path")
      ((added_count++))
    else
      gith_error "Failed to add submodule: $submodule_path"
      gith_error "Continuing with remaining submodules..."
      ((failed_count++))
    fi
  done

  gith_log "INFO" "Submodule addition complete: $added_count added, $failed_count failed"

  if [[ $added_count -gt 0 ]]; then
    STEPS_COMPLETED+=("submodule-addition")
  fi

  # Don't fail workflow if some submodules failed
  return 0
}

# Generate summary report
generate_summary_report() {
  gith_log "INFO" "=== Workflow Summary ==="

  # Show completed steps
  if [[ ${#STEPS_COMPLETED[@]} -gt 0 ]]; then
    gith_log "INFO" "Completed steps:"
    for step in "${STEPS_COMPLETED[@]}"; do
      gith_log "INFO" "  ✓ $step"
    done
  fi

  # Show skipped steps
  if [[ ${#STEPS_SKIPPED[@]} -gt 0 ]]; then
    gith_log "INFO" "Skipped steps:"
    for step in "${STEPS_SKIPPED[@]}"; do
      gith_log "INFO" "  - $step"
    done
  fi

  # Show created resources
  if [[ ${#RESOURCES_CREATED[@]} -gt 0 ]]; then
    gith_log "INFO" "Created/Modified resources:"
    for resource in "${RESOURCES_CREATED[@]}"; do
      gith_log "INFO" "  + $resource"
    done
  fi

  # Show repository information
  if [[ -n "$REPO_DIR" ]] && [[ -d "$REPO_DIR" ]]; then
    gith_log "INFO" "Repository location: $REPO_DIR"

    if gith_is_git_repo "$REPO_DIR"; then
      local current_branch
      current_branch=$(gith_get_current_branch "$REPO_DIR")
      if [[ -n "$current_branch" ]]; then
        gith_log "INFO" "Current branch: $current_branch"
      fi
    fi
  fi

  return 0
}

# Rollback workflow on failure
rollback_workflow() {
  local failed_step="$1"

  gith_log "INFO" "=== Rollback ==="
  gith_log "WARN" "Rolling back changes due to failure at: $failed_step"

  # Rollback based on failed step
  case "$failed_step" in
    "orphan-creation")
      # Delete orphan branch if it was created
      if [[ -n "$REPO_DIR" ]] && gith_is_git_repo "$REPO_DIR"; then
        if (cd "$REPO_DIR" && git show-ref --verify --quiet "refs/heads/$ORPHAN_BRANCH" 2>/dev/null); then
          gith_log "INFO" "Deleting orphan branch: $ORPHAN_BRANCH"
          (cd "$REPO_DIR" && git branch -D "$ORPHAN_BRANCH" 2>/dev/null || true)
        fi
      fi
      ;;
    "submodule-addition")
      # Submodule failures are non-fatal, no rollback needed
      gith_log "INFO" "Submodule failures are non-fatal, no rollback needed"
      ;;
  esac

  # Restore original branch and stash if saved
  if [[ -n "$REPO_DIR" ]] && gith_is_git_repo "$REPO_DIR"; then
    if [[ -n "$ORIGINAL_BRANCH" ]]; then
      gith_log "INFO" "Restoring original branch: $ORIGINAL_BRANCH"
      (cd "$REPO_DIR" && git checkout "$ORIGINAL_BRANCH" 2>/dev/null || true)

      if [[ -n "$STASH_REF" ]]; then
        gith_log "INFO" "Restoring stash: $STASH_REF"
        gith_stash_pop "$REPO_DIR" "$STASH_REF" || true
      fi
    fi
  fi

  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Validate prerequisites
  gith_validate_prerequisites --require-git --script-name "$(basename "$0")"

  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --repo-url)
        REPO_URL="$2"
        shift 2
        ;;
      --repo-http-url)
        REPO_HTTP_URL="$2"
        shift 2
        ;;
      --upstream-ssh)
        UPSTREAM_SSH="$2"
        shift 2
        ;;
      --upstream-http)
        UPSTREAM_HTTP="$2"
        shift 2
        ;;
      --repo-dir)
        REPO_DIR="$2"
        shift 2
        ;;
      --orphan-branch)
        ORPHAN_BRANCH="$2"
        shift 2
        ;;
      --submodule)
        SUBMODULES+=("$2")
        shift 2
        ;;
      --skip-main-init)
        SKIP_MAIN_INIT=1
        shift
        ;;
      --skip-orphan)
        SKIP_ORPHAN=1
        shift
        ;;
      --skip-submodules)
        SKIP_SUBMODULES=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
    esac
  done

  # Validate arguments
  validate_arguments

  # Show workflow configuration
  gith_log "INFO" "=== Repository Initialization Workflow ==="
  gith_log "INFO" "Repository URL: $REPO_URL"
  if [[ -n "$REPO_HTTP_URL" ]]; then
    gith_log "INFO" "HTTP URL: $REPO_HTTP_URL"
  fi
  if [[ -n "$UPSTREAM_SSH" ]]; then
    gith_log "INFO" "Upstream SSH: $UPSTREAM_SSH"
  fi
  if [[ -n "$UPSTREAM_HTTP" ]]; then
    gith_log "INFO" "Upstream HTTP: $UPSTREAM_HTTP"
  fi
  if [[ -n "$REPO_DIR" ]]; then
    gith_log "INFO" "Repository directory: $REPO_DIR"
  fi
  gith_log "INFO" "Orphan branch: $ORPHAN_BRANCH"
  if [[ ${#SUBMODULES[@]} -gt 0 ]]; then
    gith_log "INFO" "Submodules: ${#SUBMODULES[@]}"
  fi
  if [[ "$DRY_RUN" == "1" ]]; then
    gith_log "INFO" "Mode: DRY RUN"
  fi
  echo ""

  # Execute workflow steps
  local workflow_failed=0
  local failed_step=""

  # Step 1: Detect remote status
  if ! detect_remote_status; then
    workflow_failed=1
    failed_step="remote-detection"
  fi

  # Step 2: Initialize main branch
  if [[ $workflow_failed -eq 0 ]]; then
    if ! initialize_main_branch; then
      workflow_failed=1
      failed_step="main-init"
    fi
  fi

  # Step 3: Setup multi-remote
  if [[ $workflow_failed -eq 0 ]]; then
    if ! setup_multi_remote; then
      workflow_failed=1
      failed_step="multi-remote-setup"
    fi
  fi

  # Step 4: Create orphan branch
  if [[ $workflow_failed -eq 0 ]]; then
    if ! create_orphan_branch; then
      workflow_failed=1
      failed_step="orphan-creation"
    fi
  fi

  # Step 5: Add submodules
  if [[ $workflow_failed -eq 0 ]]; then
    if ! add_submodules; then
      # Submodule failures are non-fatal
      gith_log "WARN" "Some submodules failed to add, but continuing workflow"
    fi
  fi

  # Handle workflow failure
  if [[ $workflow_failed -eq 1 ]]; then
    gith_error "Workflow failed at step: $failed_step"
    rollback_workflow "$failed_step"
    generate_summary_report
    exit 1
  fi

  # Generate summary report
  generate_summary_report

  gith_log "INFO" "Workflow completed successfully!"
  return 0
}

# Run main function
main "$@"
