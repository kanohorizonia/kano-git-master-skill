#!/usr/bin/env bash
#
# init-kano-dev-skill.sh - Initialize a new Kano skill repository for development
#
# Purpose:
#   Initialize a new Kano skill repository with development tooling:
#   1. Clone or initialize repository
#   2. Configure multi-remote with SSH/HTTPS fallback
#   3. Initialize main branch (if remote is empty)
#   4. Create orphan tooling branch (named after the repo)
#   5. Add development skills as submodules
#
# Usage:
#   ./init-kano-dev-skill.sh --repo-ssh <url> --repo-https <url> --repo-dir <path> [options]
#
# Required Options:
#   --repo-ssh <url>        Repository SSH URL
#   --repo-https <url>      Repository HTTPS URL
#   --repo-dir <path>       Local repository directory (e.g., skills/kano)
#
# Optional Options:
#   --tooling-branch <name> Tooling branch name (default: dev/tooling)
#   --skill <ssh>:<https>:<path>  Skill to add (can be repeated)
#   --upstream-ssh <url>    Upstream SSH URL
#   --upstream-https <url>  Upstream HTTPS URL
#   --skip-main-init        Skip main branch initialization
#   --skip-tooling          Skip tooling branch creation
#   --skip-skills           Skip skill addition
#   --dry-run               Show what would be done
#   -h, --help              Show help
#
# Examples:
#   # Initialize new skill repo with development skills
#   ./init-kano-dev-skill.sh \
#     --repo-ssh git@github.com:user/kano-new-skill.git \
#     --repo-https https://github.com/user/kano-new-skill.git \
#     --repo-dir skills/new-skill \
#     --skill "git@github.com:user/skill1.git:https://github.com/user/skill1.git:skills/skill1" \
#     --skill "git@github.com:user/skill2.git:https://github.com/user/skill2.git:skills/skill2"
#
#   # Dry run to preview
#   ./init-kano-dev-skill.sh \
#     --repo-ssh git@github.com:user/kano-new-skill.git \
#     --repo-https https://github.com/user/kano-new-skill.git \
#     --repo-dir skills/new-skill \
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
REPO_SSH=""
REPO_HTTPS=""
REPO_DIR=""
UPSTREAM_SSH=""
UPSTREAM_HTTPS=""
TOOLING_BRANCH=""  # No default - will be derived from repo name
SKILLS=()
SKIP_MAIN_INIT=0
SKIP_TOOLING=0
SKIP_SKILLS=0
DRY_RUN=0

# Workflow state
ORIGINAL_DIR="$(pwd)"
REPO_EXISTS=0
REMOTE_IS_EMPTY=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") --repo-ssh <url> --repo-https <url> --repo-dir <path> [options]

Initialize a new Kano repository with tooling branch and skills.

Required Options:
  --repo-ssh <url>        Repository SSH URL
  --repo-https <url>      Repository HTTPS URL
  --repo-dir <path>       Local repository directory

Optional Options:
  --tooling-branch <name> Tooling branch name (default: dev/<repo-name>-tooling)
  --skill <ssh>:<https>:<path>  Skill to add (format: ssh_url:https_url:path)
  --upstream-ssh <url>    Upstream SSH URL
  --upstream-https <url>  Upstream HTTPS URL
  --skip-main-init        Skip main branch initialization
  --skip-tooling          Skip tooling branch creation
  --skip-skills           Skip skill addition
  --dry-run               Show what would be done
  -h, --help              Show help

Examples:
  # Initialize with skills
  $(basename "$0") \\
    --repo-ssh git@github.com:user/repo.git \\
    --repo-https https://github.com/user/repo.git \\
    --repo-dir skills/kano \\
    --skill "git@github.com:user/skill1.git:https://github.com/user/skill1.git:skills/skill1"

EOF
}

# Parse command line arguments
parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --repo-ssh)
        REPO_SSH="$2"
        shift 2
        ;;
      --repo-https)
        REPO_HTTPS="$2"
        shift 2
        ;;
      --repo-dir)
        REPO_DIR="$2"
        shift 2
        ;;
      --upstream-ssh)
        UPSTREAM_SSH="$2"
        shift 2
        ;;
      --upstream-https)
        UPSTREAM_HTTPS="$2"
        shift 2
        ;;
      --tooling-branch)
        TOOLING_BRANCH="$2"
        shift 2
        ;;
      --skill)
        SKILLS+=("$2")
        shift 2
        ;;
      --skip-main-init)
        SKIP_MAIN_INIT=1
        shift
        ;;
      --skip-tooling)
        SKIP_TOOLING=1
        shift
        ;;
      --skip-skills)
        SKIP_SKILLS=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      *)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
    esac
  done
}

# Validate required arguments
validate_args() {
  if [[ -z "$REPO_SSH" ]]; then
    gith_error "Error: --repo-ssh is required"
    usage
    exit 1
  fi
  
  if [[ -z "$REPO_HTTPS" ]]; then
    gith_error "Error: --repo-https is required"
    usage
    exit 1
  fi
  
  if [[ -z "$REPO_DIR" ]]; then
    gith_error "Error: --repo-dir is required"
    usage
    exit 1
  fi
  
  # Derive tooling branch name from repo name if not specified
  if [[ -z "$TOOLING_BRANCH" ]]; then
    # Extract repo name from SSH URL (e.g., git@github.com:user/kano-agent-skill.git -> kano-agent-skill)
    local repo_name
    repo_name=$(basename "$REPO_SSH" .git)
    TOOLING_BRANCH="dev/${repo_name}-tooling"
    gith_log "INFO" "Using derived tooling branch name: $TOOLING_BRANCH"
  fi
}

# Check if remote repository is empty
check_remote_status() {
  gith_log "INFO" "Checking remote repository status..."
  
  if git ls-remote "$REPO_HTTPS" HEAD >/dev/null 2>&1; then
    local ref_count
    ref_count=$(git ls-remote "$REPO_HTTPS" 2>/dev/null | wc -l)
    
    if [[ "$ref_count" -eq 0 ]]; then
      REMOTE_IS_EMPTY=1
      gith_log "INFO" "  Remote repository is empty"
    else
      REMOTE_IS_EMPTY=0
      gith_log "INFO" "  Remote repository has content ($ref_count references)"
    fi
  else
    gith_error "Error: Cannot access remote repository: $REPO_HTTPS"
    exit 1
  fi
}

# Clone or initialize repository
init_repository() {
  gith_log "INFO" "Step 1: Initialize repository"
  
  if [[ -d "$REPO_DIR" ]]; then
    REPO_EXISTS=1
    gith_log "INFO" "  Repository directory already exists: $REPO_DIR"
    cd "$REPO_DIR"
    
    if [[ ! -d ".git" ]]; then
      gith_error "Error: Directory exists but is not a Git repository"
      exit 1
    fi
  else
    REPO_EXISTS=0
    
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY-RUN] Would clone: $REPO_HTTPS to $REPO_DIR"
    else
      gith_log "INFO" "  Cloning repository..."
      git clone "$REPO_HTTPS" "$REPO_DIR"
      cd "$REPO_DIR"
    fi
  fi
}

# Configure multi-remote
configure_remotes() {
  gith_log "INFO" "Step 2: Configure multi-remote"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would configure remotes:"
    echo "  origin-ssh: $REPO_SSH"
    echo "  origin-https: $REPO_HTTPS"
    if [[ -n "$UPSTREAM_SSH" ]]; then
      echo "  upstream-ssh: $UPSTREAM_SSH"
      echo "  upstream-https: $UPSTREAM_HTTPS"
    fi
  else
    local cmd="$SCRIPT_DIR/../submodules/kog-submodule.sh add --remote origin --ssh \"$REPO_SSH\" --https \"$REPO_HTTPS\""
    
    if [[ -n "$UPSTREAM_SSH" ]]; then
      cmd="$cmd --remote upstream --ssh \"$UPSTREAM_SSH\" --https \"$UPSTREAM_HTTPS\""
    fi
    
    cmd="$cmd --push-remote origin --protocol auto"
    
    gith_log "INFO" "  Configuring root repo remotes..."
    eval "$cmd"
  fi
}

# Initialize main branch if remote is empty
init_main_branch() {
  if [[ "$SKIP_MAIN_INIT" -eq 1 ]]; then
    gith_log "INFO" "Step 3: Initialize main branch (SKIPPED)"
    return 0
  fi
  
  gith_log "INFO" "Step 3: Initialize main branch"
  
  if [[ "$REMOTE_IS_EMPTY" -eq 1 ]]; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY-RUN] Would initialize main branch with README.md"
    else
      gith_log "INFO" "  Creating initial README.md..."
      echo "# $(basename "$REPO_DIR")" > README.md
      echo "" >> README.md
      echo "This repository was initialized using kano-git-master-skill." >> README.md
      
      git add README.md
      git commit -m "chore: Initialize repository"
      git push origin main
      
      gith_log "INFO" "  Main branch initialized and pushed"
    fi
  else
    gith_log "INFO" "  Remote has content, skipping main branch initialization"
  fi
}

# Create tooling branch
create_tooling_branch() {
  if [[ "$SKIP_TOOLING" -eq 1 ]]; then
    gith_log "INFO" "Step 4: Create tooling branch (SKIPPED)"
    return 0
  fi
  
  gith_log "INFO" "Step 4: Create tooling branch: $TOOLING_BRANCH"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would create orphan branch: $TOOLING_BRANCH"
  else
    "$SCRIPT_DIR/../core/create-orphan-branch.sh" \
      --branch "$TOOLING_BRANCH" \
      --file README.md \
      --content "# Development Tooling\n\nThis branch contains development tools and skills for this project." \
      --message "chore: Initialize development tooling branch" \
      --push
    
    gith_log "INFO" "  Tooling branch created and pushed"
  fi
}

# Add skills as submodules
add_skills() {
  if [[ "$SKIP_SKILLS" -eq 1 ]]; then
    gith_log "INFO" "Step 5: Add skills (SKIPPED)"
    return 0
  fi
  
  if [[ ${#SKILLS[@]} -eq 0 ]]; then
    gith_log "INFO" "Step 5: Add skills (no skills specified)"
    return 0
  fi
  
  gith_log "INFO" "Step 5: Add skills as submodules"
  
  # Ensure we're on the tooling branch
  if [[ "$DRY_RUN" -eq 0 ]]; then
    git checkout "$TOOLING_BRANCH"
  fi
  
  for skill in "${SKILLS[@]}"; do
    # Parse skill format: ssh_url:https_url:path
    IFS=':' read -r skill_ssh skill_https skill_path <<< "$skill"
    
    if [[ -z "$skill_ssh" || -z "$skill_https" || -z "$skill_path" ]]; then
      gith_error "Error: Invalid skill format: $skill"
      gith_error "Expected format: ssh_url:https_url:path"
      continue
    fi
    
    gith_log "INFO" "  Adding skill: $skill_path"
    
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY-RUN] Would add skill:"
      echo "  Path: $skill_path"
      echo "  SSH: $skill_ssh"
      echo "  HTTPS: $skill_https"
    else
      "$SCRIPT_DIR/../submodules/kog-submodule.sh" add \
        --path "$skill_path" \
        --remote origin \
          --ssh "$skill_ssh" \
          --https "$skill_https" \
        --push-remote origin \
        --protocol auto
    fi
  done
  
  # Commit and push
  if [[ "$DRY_RUN" -eq 0 && ${#SKILLS[@]} -gt 0 ]]; then
    git add .gitmodules
    for skill in "${SKILLS[@]}"; do
      IFS=':' read -r _ _ skill_path <<< "$skill"
      git add "$skill_path" 2>/dev/null || true
    done
    
    git commit -m "feat: Add development skills as submodules"
    git push origin "$TOOLING_BRANCH"
    
    gith_log "INFO" "  Skills added and pushed"
  fi
}

# Generate summary
generate_summary() {
  echo ""
  echo "========================================"
  echo "Workflow Summary"
  echo "========================================"
  echo "Repository: $REPO_DIR"
  echo "Remote: $REPO_HTTPS"
  echo "Tooling branch: $TOOLING_BRANCH"
  echo "Skills added: ${#SKILLS[@]}"
  echo ""
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "This was a DRY RUN - no changes were made"
  else
    echo "Initialization complete!"
    echo ""
    echo "Next steps:"
    echo "  cd $REPO_DIR"
    echo "  git checkout $TOOLING_BRANCH  # Switch to tooling branch"
    echo "  git checkout main             # Switch back to main"
  fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  parse_args "$@"
  validate_args
  
  echo "========================================"
  echo "Kano Repository Initialization"
  echo "========================================"
  echo ""
  
  check_remote_status
  init_repository
  configure_remotes
  init_main_branch
  create_tooling_branch
  add_skills
  generate_summary
  
  cd "$ORIGINAL_DIR"
}

main "$@"
