#!/usr/bin/env bash
#
# init-test-repo.sh - Initialize test repository with sample content
#
# Purpose:
#   Create a local repository with initial commits and push to remote
#   Useful for setting up test repositories from scratch
#
# Usage:
#   ./init-test-repo.sh <remote-url> [options]
#
# Arguments:
#   remote-url        Remote repository URL (e.g., git@github.com:user/repo.git)
#
# Options:
#   --dir <path>      Local directory (default: temp directory)
#   --branch <name>   Default branch name (default: main)
#   --commits <n>     Number of initial commits (default: 5)
#   --with-submodule  Add a test submodule
#   --force           Force push (use with caution)
#   -h, --help        Show help
#
# Examples:
#   # Basic initialization
#   ./init-test-repo.sh git@github.com:user/test-repo.git
#
#   # Custom directory and branch
#   ./init-test-repo.sh git@github.com:user/test-repo.git \
#     --dir /tmp/my-test-repo \
#     --branch main
#
#   # With more commits
#   ./init-test-repo.sh git@github.com:user/test-repo.git --commits 10
#
#   # With submodule
#   ./init-test-repo.sh git@github.com:user/test-repo.git --with-submodule
#

set -euo pipefail

# Configuration
REMOTE_URL=""
LOCAL_DIR=""
BRANCH_NAME="main"
NUM_COMMITS=5
WITH_SUBMODULE=0
FORCE_PUSH=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") <remote-url> [options]

Initialize a test repository with sample content and push to remote.

Arguments:
  remote-url        Remote repository URL

Options:
  --dir <path>      Local directory (default: temp directory)
  --branch <name>   Default branch name (default: main)
  --commits <n>     Number of initial commits (default: 5)
  --with-submodule  Add a test submodule
  --force           Force push (use with caution)
  -h, --help        Show help

Examples:
  # Initialize test repo
  ./init-test-repo.sh git@github.com:dorgonman/kano-git-master-skill-demo.git

  # Custom directory
  ./init-test-repo.sh git@github.com:user/repo.git --dir /tmp/test-repo

  # With 10 commits
  ./init-test-repo.sh git@github.com:user/repo.git --commits 10

  # With submodule
  ./init-test-repo.sh git@github.com:user/repo.git --with-submodule

What this script creates:
  - README.md with project description
  - .gitignore with common patterns
  - src/ directory with sample code
  - docs/ directory with documentation
  - Multiple commits with realistic history
  - Optional: test submodule

EOF
}

log_info() {
  echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
  echo -e "${GREEN}[SUCCESS]${NC} $*"
}

log_error() {
  echo -e "${RED}[ERROR]${NC} $*"
}

log_warn() {
  echo -e "${YELLOW}[WARN]${NC} $*"
}

create_readme() {
  local dir="$1"
  cat > "$dir/README.md" <<'EOF'
# Test Repository

This is a test repository for Git Master Skill testing.

## Purpose

This repository is used for testing various Git operations:
- Repository updates
- Branch comparisons
- Cherry-picking
- Submodule handling
- Multi-repository workflows

## Structure

```
.
├── README.md           # This file
├── src/                # Source code
│   ├── main.sh         # Main script
│   └── utils.sh        # Utility functions
├── docs/               # Documentation
│   └── guide.md        # User guide
└── tests/              # Test files
    └── test.sh         # Test script
```

## Usage

This is a test repository. See the main Git Master Skill documentation for usage.

## License

MIT License
EOF
}

create_gitignore() {
  local dir="$1"
  cat > "$dir/.gitignore" <<'EOF'
# OS files
.DS_Store
Thumbs.db

# Editor files
*.swp
*.swo
*~
.vscode/
.idea/

# Build artifacts
dist/
build/
*.o
*.so
*.dylib

# Dependencies
node_modules/
vendor/

# Logs
*.log
logs/

# Temporary files
tmp/
temp/
*.tmp

# Environment
.env
.env.local
EOF
}

create_source_files() {
  local dir="$1"
  
  mkdir -p "$dir/src"
  
  cat > "$dir/src/main.sh" <<'EOF'
#!/usr/bin/env bash
#
# main.sh - Main script for test repository
#

set -euo pipefail

source "$(dirname "$0")/utils.sh"

main() {
  echo "Test Repository - Main Script"
  echo "=============================="
  echo ""
  
  greet "World"
  
  echo ""
  echo "Version: 1.0.0"
}

main "$@"
EOF
  
  cat > "$dir/src/utils.sh" <<'EOF'
#!/usr/bin/env bash
#
# utils.sh - Utility functions
#

greet() {
  local name="${1:-User}"
  echo "Hello, $name!"
}

log_message() {
  local level="$1"
  shift
  echo "[$level] $*"
}
EOF
  
  chmod +x "$dir/src/main.sh"
}

create_docs() {
  local dir="$1"
  
  mkdir -p "$dir/docs"
  
  cat > "$dir/docs/guide.md" <<'EOF'
# User Guide

## Getting Started

This is a test repository for Git Master Skill.

### Installation

```bash
git clone <repository-url>
cd <repository-name>
```

### Running

```bash
bash src/main.sh
```

## Features

- Sample source code
- Documentation
- Test scripts
- Git workflow examples

## Contributing

This is a test repository. Contributions are welcome for testing purposes.

## Support

For issues, please refer to the main Git Master Skill documentation.
EOF
}

create_tests() {
  local dir="$1"
  
  mkdir -p "$dir/tests"
  
  cat > "$dir/tests/test.sh" <<'EOF'
#!/usr/bin/env bash
#
# test.sh - Test script
#

set -euo pipefail

echo "Running tests..."
echo ""

# Test 1: Check main script exists
if [[ -f "src/main.sh" ]]; then
  echo "✓ Test 1: main.sh exists"
else
  echo "✗ Test 1: main.sh missing"
  exit 1
fi

# Test 2: Check utils script exists
if [[ -f "src/utils.sh" ]]; then
  echo "✓ Test 2: utils.sh exists"
else
  echo "✗ Test 2: utils.sh missing"
  exit 1
fi

# Test 3: Check README exists
if [[ -f "README.md" ]]; then
  echo "✓ Test 3: README.md exists"
else
  echo "✗ Test 3: README.md missing"
  exit 1
fi

echo ""
echo "All tests passed!"
EOF
  
  chmod +x "$dir/tests/test.sh"
}

create_additional_commits() {
  local dir="$1"
  local num="$2"
  
  cd "$dir"
  
  for i in $(seq 1 "$num"); do
    case $i in
      1)
        echo "# Feature $i" >> README.md
        git add README.md
        git commit -m "docs: Add feature $i to README"
        ;;
      2)
        echo "" >> src/utils.sh
        echo "# Feature $i" >> src/utils.sh
        git add src/utils.sh
        git commit -m "feat: Add feature $i to utils"
        ;;
      3)
        echo "" >> docs/guide.md
        echo "## Feature $i" >> docs/guide.md
        git add docs/guide.md
        git commit -m "docs: Document feature $i"
        ;;
      4)
        echo "# Test $i" >> tests/test.sh
        git add tests/test.sh
        git commit -m "test: Add test $i"
        ;;
      *)
        echo "# Update $i" >> README.md
        git add README.md
        git commit -m "chore: Update $i"
        ;;
    esac
    
    log_info "Created commit $i/$num"
  done
}

add_submodule() {
  local dir="$1"
  
  cd "$dir"
  
  log_info "Adding test submodule..."
  
  # Create a simple submodule repository
  local submodule_dir="$dir/.submodule-temp"
  mkdir -p "$submodule_dir"
  cd "$submodule_dir"
  
  git init
  echo "# Submodule" > README.md
  git add README.md
  git commit -m "Initial commit"
  
  cd "$dir"
  git submodule add "$submodule_dir" vendor/lib
  git commit -m "feat: Add vendor/lib submodule"
  
  log_success "Submodule added"
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
      --dir)
        if [[ -z "${2:-}" ]]; then
          log_error "Option --dir requires an argument"
          usage
          exit 1
        fi
        LOCAL_DIR="$2"
        shift 2
        ;;
      --branch)
        if [[ -z "${2:-}" ]]; then
          log_error "Option --branch requires an argument"
          usage
          exit 1
        fi
        BRANCH_NAME="$2"
        shift 2
        ;;
      --commits)
        if [[ -z "${2:-}" ]]; then
          log_error "Option --commits requires an argument"
          usage
          exit 1
        fi
        NUM_COMMITS="$2"
        shift 2
        ;;
      --with-submodule)
        WITH_SUBMODULE=1
        shift
        ;;
      --force)
        FORCE_PUSH=1
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
  
  # Get remote URL from positional arguments
  if [[ ${#positional_args[@]} -lt 1 ]]; then
    log_error "Remote URL is required"
    usage
    exit 1
  fi
  
  REMOTE_URL="${positional_args[0]}"
  
  # Set default local directory if not specified
  if [[ -z "$LOCAL_DIR" ]]; then
    LOCAL_DIR="$(mktemp -d)"
    log_info "Using temporary directory: $LOCAL_DIR"
  fi
  
  # Validate inputs
  if ! [[ "$NUM_COMMITS" =~ ^[0-9]+$ ]] || [[ "$NUM_COMMITS" -lt 1 ]]; then
    log_error "Number of commits must be a positive integer"
    exit 1
  fi
  
  echo ""
  echo "=========================================="
  echo "  Initialize Test Repository"
  echo "=========================================="
  echo ""
  log_info "Remote URL: $REMOTE_URL"
  log_info "Local directory: $LOCAL_DIR"
  log_info "Branch name: $BRANCH_NAME"
  log_info "Number of commits: $NUM_COMMITS"
  log_info "With submodule: $([[ $WITH_SUBMODULE -eq 1 ]] && echo 'Yes' || echo 'No')"
  echo ""
  
  # Create local directory
  mkdir -p "$LOCAL_DIR"
  cd "$LOCAL_DIR"
  
  # Initialize git repository
  log_info "Initializing git repository..."
  git init
  git checkout -b "$BRANCH_NAME"
  
  # Create initial structure
  log_info "Creating initial structure..."
  create_readme "$LOCAL_DIR"
  create_gitignore "$LOCAL_DIR"
  create_source_files "$LOCAL_DIR"
  create_docs "$LOCAL_DIR"
  create_tests "$LOCAL_DIR"
  
  # Initial commit
  log_info "Creating initial commit..."
  git add .
  git commit -m "Initial commit: Add project structure

- Add README.md with project description
- Add .gitignore with common patterns
- Add src/ with main.sh and utils.sh
- Add docs/ with user guide
- Add tests/ with test script"
  
  log_success "Initial commit created"
  
  # Create additional commits
  if [[ "$NUM_COMMITS" -gt 0 ]]; then
    log_info "Creating $NUM_COMMITS additional commits..."
    create_additional_commits "$LOCAL_DIR" "$NUM_COMMITS"
    log_success "Additional commits created"
  fi
  
  # Add submodule if requested
  if [[ "$WITH_SUBMODULE" -eq 1 ]]; then
    add_submodule "$LOCAL_DIR"
  fi
  
  # Add remote
  log_info "Adding remote: $REMOTE_URL"
  git remote add origin "$REMOTE_URL"
  
  # Push to remote
  log_info "Pushing to remote..."
  if [[ "$FORCE_PUSH" -eq 1 ]]; then
    log_warn "Force pushing to remote..."
    git push -f origin "$BRANCH_NAME"
  else
    git push -u origin "$BRANCH_NAME"
  fi
  
  log_success "Repository pushed to remote"
  
  # Summary
  echo ""
  echo "=========================================="
  echo "  Summary"
  echo "=========================================="
  echo ""
  log_success "Test repository initialized successfully!"
  echo ""
  echo "Repository details:"
  echo "  Remote: $REMOTE_URL"
  echo "  Local: $LOCAL_DIR"
  echo "  Branch: $BRANCH_NAME"
  echo "  Commits: $((NUM_COMMITS + 1))"
  echo ""
  echo "Repository structure:"
  cd "$LOCAL_DIR"
  tree -L 2 -a 2>/dev/null || find . -maxdepth 2 -not -path '*/\.git/*' | sort
  echo ""
  echo "Git log:"
  git log --oneline --graph --all -10
  echo ""
  echo "To clone this repository:"
  echo "  git clone $REMOTE_URL"
  echo ""
  
  if [[ "$LOCAL_DIR" =~ ^/tmp ]]; then
    log_info "Local directory is temporary and will be cleaned up on reboot"
    log_info "To keep it, copy to a permanent location"
  fi
}

# Run main function
main "$@"
