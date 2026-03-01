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
#   ./init-kano-dev-skill.sh --repo-dir <path> [--repo-ssh <url>] [--repo-https <url>] [options]
#
# Required Options:
#   --repo-dir <path>       Local repository directory (e.g., skills/kano)
#   --repo-ssh <url>        Repository SSH URL (required if --repo-https not provided)
#   --repo-https <url>      Repository HTTPS URL (required if --repo-ssh not provided)
#
# Optional Options:
#   --tooling-branch <name> Tooling branch name (default: dev/<repo-name>-tooling)
#   --skill <ssh>|<https>|<path>  Skill to add (can be repeated, use | as delimiter)
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
#     --skill "git@github.com:user/skill1.git|https://github.com/user/skill1.git|skills/skill1" \
#     --skill "git@github.com:user/skill2.git|https://github.com/user/skill2.git|skills/skill2"
#
#   # Dry run to preview
#   ./init-kano-dev-skill.sh \
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
FORCE_OVERWRITE_TOOLING=0
UPDATE_TOOLING=0
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
Usage: $(basename "$0") --repo-dir <path> [--repo-ssh <url>] [--repo-https <url>] [options]

Initialize a new Kano repository with tooling branch and skills.

Required Options:
  --repo-dir <path>       Local repository directory
  --repo-ssh <url>        Repository SSH URL (required if --repo-https not provided)
  --repo-https <url>      Repository HTTPS URL (required if --repo-ssh not provided)

Optional Options:
  --tooling-branch <name> Tooling branch name (default: dev/<repo-name>-tooling)
  --skill <ssh>|<https>|<path>  Skill to add (format: ssh_url|https_url|path)
  --upstream-ssh <url>    Upstream SSH URL
  --upstream-https <url>  Upstream HTTPS URL
  --skip-main-init        Skip main branch initialization
  --skip-tooling          Skip tooling branch creation
  --skip-skills           Skip skill addition
  --update-tooling        Update existing tooling branch (rebase onto origin/<tooling-branch>)
  --dry-run               Show what would be done
  -h, --help              Show help

Examples:
  # Initialize with skills
  $(basename "$0") \\
    --repo-ssh git@github.com:user/repo.git \\
    --repo-https https://github.com/user/repo.git \\
    --repo-dir skills/kano \\
    --skill "git@github.com:user/skill1.git|https://github.com/user/skill1.git|skills/skill1"

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
      --update-tooling)
        UPDATE_TOOLING=1
        shift
        ;;
      --force-overwrite-tooling)
        FORCE_OVERWRITE_TOOLING=1
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
  if [[ -z "$REPO_SSH" && -z "$REPO_HTTPS" ]]; then
    gith_error "Error: --repo-ssh or --repo-https is required"
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
    local repo_name
    if [[ -n "$REPO_SSH" ]]; then
      repo_name=$(basename "$REPO_SSH" .git)
    else
      repo_name=$(basename "$REPO_HTTPS" .git)
    fi
    TOOLING_BRANCH="dev/${repo_name}-tooling"
    gith_log "INFO" "Using derived tooling branch name: $TOOLING_BRANCH"
  fi
}

# Check if remote repository is empty
check_remote_status() {
  gith_log "INFO" "Checking remote repository status..."

  local check_urls=()
  if [[ -n "$REPO_HTTPS" ]]; then
    check_urls+=("$REPO_HTTPS")
  fi
  if [[ -n "$REPO_SSH" ]]; then
    check_urls+=("$REPO_SSH")
  fi

  local check_url=""
  local ref_count=""
  for url in "${check_urls[@]}"; do
    if git ls-remote "$url" HEAD >/dev/null 2>&1; then
      check_url="$url"
      ref_count=$(git ls-remote "$url" 2>/dev/null | wc -l)
      break
    fi
  done

  if [[ -z "$check_url" ]]; then
    gith_error "Error: Cannot access remote repository via provided URLs"
    exit 1
  fi

  if [[ "$ref_count" -eq 0 ]]; then
    REMOTE_IS_EMPTY=1
    gith_log "INFO" "  Remote repository is empty"
  else
    REMOTE_IS_EMPTY=0
    gith_log "INFO" "  Remote repository has content ($ref_count references)"
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

    local clone_url=""
    if [[ -n "$REPO_SSH" ]]; then
      clone_url="$REPO_SSH"
    elif [[ -n "$REPO_HTTPS" ]]; then
      clone_url="$REPO_HTTPS"
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY-RUN] Would clone: $clone_url to $REPO_DIR"
    else
      gith_log "INFO" "  Cloning repository..."
      git clone "$clone_url" "$REPO_DIR"
      cd "$REPO_DIR"
    fi
  fi
}

# Configure multi-remote
configure_remotes() {
  gith_log "INFO" "Step 2: Configure multi-remote"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would configure remotes:"
    if [[ -n "$REPO_SSH" && -n "$REPO_HTTPS" ]]; then
      echo "  origin-ssh: $REPO_SSH"
      echo "  origin-http: $REPO_HTTPS"
    elif [[ -n "$REPO_SSH" ]]; then
      echo "  origin: $REPO_SSH"
    else
      echo "  origin: $REPO_HTTPS"
    fi

    if [[ -n "$UPSTREAM_SSH" && -n "$UPSTREAM_HTTPS" ]]; then
      echo "  upstream-ssh: $UPSTREAM_SSH"
      echo "  upstream-http: $UPSTREAM_HTTPS"
    elif [[ -n "$UPSTREAM_SSH" ]]; then
      echo "  upstream: $UPSTREAM_SSH"
    elif [[ -n "$UPSTREAM_HTTPS" ]]; then
      echo "  upstream: $UPSTREAM_HTTPS"
    fi
  else
    gith_log "INFO" "  Configuring root repo remotes..."
    
    local setup_cmd=("$SCRIPT_DIR/../core/setup-multi-remote.sh")
    if [[ -n "$REPO_SSH" ]]; then
      setup_cmd+=("--origin-ssh" "$REPO_SSH")
    fi
    if [[ -n "$REPO_HTTPS" ]]; then
      setup_cmd+=("--origin-http" "$REPO_HTTPS")
    fi

    if [[ -n "$UPSTREAM_SSH" ]]; then
      setup_cmd+=("--upstream-ssh" "$UPSTREAM_SSH")
    fi
    if [[ -n "$UPSTREAM_HTTPS" ]]; then
      setup_cmd+=("--upstream-http" "$UPSTREAM_HTTPS")
    fi
    
    setup_cmd+=("--dir" "$REPO_DIR")
    if [[ -n "$REPO_SSH" && -n "$REPO_HTTPS" ]]; then
      setup_cmd+=("--validate")
    elif [[ -n "$UPSTREAM_SSH" && -n "$UPSTREAM_HTTPS" ]]; then
      setup_cmd+=("--validate")
    fi
    
    "${setup_cmd[@]}"
  fi
}

# Initialize main branch if remote is empty
push_with_fallback() {
  local branch="$1"
  local repo_dir="$2"

  if [[ -z "$branch" ]]; then
    gith_error "push_with_fallback: branch name is required"
    return 1
  fi

  if [[ ! -d "$repo_dir" ]]; then
    gith_error "push_with_fallback: repository directory does not exist: $repo_dir"
    return 1
  fi

  if ! gith_is_git_repo "$repo_dir"; then
    gith_error "push_with_fallback: not a git repository: $repo_dir"
    return 1
  fi

  local ssh_remote=""
  local http_remote=""

  if (cd "$repo_dir" && git remote get-url origin-ssh >/dev/null 2>&1); then
    ssh_remote="origin-ssh"
  elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
    local origin_url
    origin_url=$(cd "$repo_dir" && git remote get-url origin)
    if [[ "$origin_url" =~ ^(git@|file://) ]]; then
      ssh_remote="origin"
    fi
  fi

  if (cd "$repo_dir" && git remote get-url origin-http >/dev/null 2>&1); then
    http_remote="origin-http"
  elif (cd "$repo_dir" && git remote get-url origin >/dev/null 2>&1); then
    local origin_url
    origin_url=$(cd "$repo_dir" && git remote get-url origin)
    if [[ "$origin_url" =~ ^https?:// ]]; then
      http_remote="origin"
    fi
  fi

  if [[ -n "$ssh_remote" ]]; then
    gith_log "INFO" "Attempting push to $ssh_remote..."
    if (cd "$repo_dir" && git push -u "$ssh_remote" "$branch" >/dev/null 2>&1); then
      gith_log "INFO" "Successfully pushed to $ssh_remote"
      return 0
    else
      gith_log "WARN" "SSH push failed"
    fi
  fi

  if [[ -n "$http_remote" ]]; then
    gith_log "INFO" "Falling back to $http_remote..."
    if (cd "$repo_dir" && git push -u "$http_remote" "$branch" >/dev/null 2>&1); then
      gith_log "INFO" "Successfully pushed to $http_remote"
      return 0
    else
      gith_log "WARN" "HTTP push failed"
    fi
  fi

  gith_error "Failed to push branch '$branch' to any remote"
  if [[ -z "$ssh_remote" && -z "$http_remote" ]]; then
    gith_error "No suitable remotes found for push operation"
    gith_error "Expected remotes: origin-ssh, origin-http, or origin"
  fi

  return 1
}

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
      if ! push_with_fallback "main" "$REPO_DIR"; then
        gith_error "Failed to push main branch"
        exit 1
      fi
      
      gith_log "INFO" "  Main branch initialized and pushed"
    fi
  else
    gith_log "INFO" "  Remote has content, skipping main branch initialization"
  fi
}

update_tooling_branch() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would update tooling branch via rebase: $TOOLING_BRANCH"
    return 0
  fi

  gith_log "INFO" "Updating tooling branch: $TOOLING_BRANCH"
  (cd "$REPO_DIR" && git fetch origin)

  if (cd "$REPO_DIR" && git show-ref --verify --quiet "refs/heads/$TOOLING_BRANCH"); then
    (cd "$REPO_DIR" && git checkout "$TOOLING_BRANCH")
  elif (cd "$REPO_DIR" && git ls-remote --heads origin "$TOOLING_BRANCH" 2>/dev/null | grep -q "$TOOLING_BRANCH"); then
    (cd "$REPO_DIR" && git checkout -B "$TOOLING_BRANCH" "origin/$TOOLING_BRANCH")
  else
    gith_error "Tooling branch not found locally or on origin: $TOOLING_BRANCH"
    exit 1
  fi

  (cd "$REPO_DIR" && git rebase "origin/$TOOLING_BRANCH")
}

create_tooling_branch() {
  if [[ "$SKIP_TOOLING" -eq 1 ]]; then
    gith_log "INFO" "Step 4: Create tooling branch (SKIPPED)"
    return 0
  fi
  
  gith_log "INFO" "Step 4: Create tooling branch: $TOOLING_BRANCH"
  
  if [[ "$UPDATE_TOOLING" -eq 1 ]]; then
    update_tooling_branch
    return 0
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY-RUN] Would create orphan branch: $TOOLING_BRANCH"
    return 0
  fi

  local create_args=(
    "$SCRIPT_DIR/../core/create-orphan-branch.sh"
    --branch "$TOOLING_BRANCH"
    --file README.md
    --content "# Development Tooling\n\nThis branch contains development tools and skills for this project."
    --message "chore: Initialize development tooling branch"
  )

  if [[ "$FORCE_OVERWRITE_TOOLING" -eq 1 ]]; then
    create_args+=("--force-overwrite-branch")
  fi

  "${create_args[@]}"

  if ! push_with_fallback "$TOOLING_BRANCH" "$REPO_DIR"; then
    gith_error "Failed to push tooling branch"
    exit 1
  fi
  
  gith_log "INFO" "  Tooling branch created and pushed"
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
    # Parse skill format: ssh_url|https_url|path
    # Using | delimiter to avoid conflicts with : in git URLs
    IFS='|' read -r skill_ssh skill_https skill_path <<< "$skill"
    
    if [[ -z "$skill_ssh" || -z "$skill_https" || -z "$skill_path" ]]; then
      gith_error "Error: Invalid skill format: $skill"
      gith_error "Expected format: ssh_url|https_url|path"
      continue
    fi
    
    gith_log "INFO" "  Adding skill: $skill_path"
    
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY-RUN] Would add skill:"
      echo "  Path: $skill_path"
      echo "  SSH: $skill_ssh"
      echo "  HTTPS: $skill_https"
    else
      if (cd "$REPO_DIR" && git submodule status -- "$skill_path" >/dev/null 2>&1); then
        gith_log "INFO" "  Skipping existing submodule: $skill_path"
        continue
      fi

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
       IFS='|' read -r _ _ skill_path <<< "$skill"
       git add "$skill_path" 2>/dev/null || true
     done
    
    git commit -m "feat: Add development skills as submodules"
    if ! push_with_fallback "$TOOLING_BRANCH" "$REPO_DIR"; then
      gith_error "Failed to push tooling branch"
      exit 1
    fi
    
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
  if [[ -n "$REPO_SSH" && -n "$REPO_HTTPS" ]]; then
    echo "Remote (ssh): $REPO_SSH"
    echo "Remote (https): $REPO_HTTPS"
  elif [[ -n "$REPO_SSH" ]]; then
    echo "Remote: $REPO_SSH"
  else
    echo "Remote: $REPO_HTTPS"
  fi
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
