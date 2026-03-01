#!/usr/bin/env bash
# Helper functions for git-p4 operations
# Source this file: source "$(dirname "$0")/../lib/p4-helpers.sh"

# Check if Python 3 is available
check_python3() {
    local python_cmd=""
    
    # Try python3 first
    if command -v python3 > /dev/null 2>&1; then
        python_cmd="python3"
    # Try python and check version
    elif command -v python > /dev/null 2>&1; then
        local version=$(python --version 2>&1 | grep -oP 'Python \K[0-9]+')
        if [[ "$version" == "3" ]]; then
            python_cmd="python"
        fi
    fi
    
    if [[ -z "$python_cmd" ]]; then
        echo "Error: Python 3 is required for git-p4 operations" >&2
        echo "" >&2
        echo "git-p4 requires Python 3.x (Python 2 is not supported)" >&2
        echo "" >&2
        echo "Please install Python 3:" >&2
        echo "  - Windows: https://www.python.org/downloads/" >&2
        echo "  - macOS: brew install python3" >&2
        echo "  - Linux: apt-get install python3 or yum install python3" >&2
        echo "" >&2
        echo "After installation, verify with: python3 --version" >&2
        return 1
    fi
    
    echo "$python_cmd"
    return 0
}

# Check if git-p4 is available
check_git_p4() {
    if ! git p4 --help > /dev/null 2>&1; then
        echo "Error: git-p4 is not available" >&2
        echo "" >&2
        echo "git-p4 is usually included with Git, but may need to be installed separately." >&2
        echo "" >&2
        echo "Installation:" >&2
        echo "  - Windows: Included with Git for Windows" >&2
        echo "  - macOS: brew install git-p4" >&2
        echo "  - Linux: apt-get install git-p4 or yum install git-p4" >&2
        echo "" >&2
        echo "Verify with: git p4 --help" >&2
        return 1
    fi
    
    return 0
}

# Validate git-p4 environment
validate_p4_environment() {
    # Check Python 3
    local python_cmd
    python_cmd=$(check_python3) || return 1
    
    # Check git-p4
    check_git_p4 || return 1
    
    # Check if P4PORT is set (optional, but helpful)
    if [[ -z "${P4PORT:-}" ]]; then
        echo "Warning: P4PORT environment variable is not set" >&2
        echo "You may need to set it for Perforce operations:" >&2
        echo "  export P4PORT=perforce:1666" >&2
        echo "" >&2
    fi
    
    return 0
}

# Extract git-p4 metadata from commit message
extract_p4_metadata() {
    local commit="${1:-HEAD}"
    
    # Get commit message
    local message=$(git log -1 --format=%B "$commit")
    
    # Extract depot-paths
    local depot_paths=$(echo "$message" | grep -oP '\[git-p4: depot-paths = "\K[^"]+' || echo "")
    
    # Extract change number
    local change=$(echo "$message" | grep -oP 'change = \K[0-9]+' || echo "")
    
    # Output as key=value pairs
    if [[ -n "$depot_paths" ]]; then
        echo "depot_paths=$depot_paths"
    fi
    if [[ -n "$change" ]]; then
        echo "change=$change"
    fi
}

# Check if commit has git-p4 metadata
has_p4_metadata() {
    local commit="${1:-HEAD}"
    local message=$(git log -1 --format=%B "$commit")
    
    if echo "$message" | grep -q '\[git-p4:'; then
        return 0
    else
        return 1
    fi
}

# Get P4 depot path from config
get_p4_depot_path() {
    git config --get git-p4.depotPath || echo ""
}

# Get P4 client name from config
get_p4_client() {
    git config --get git-p4.client || echo ""
}

# Show P4 configuration
show_p4_config() {
    echo "Git-P4 Configuration:"
    echo "  Depot Path: $(get_p4_depot_path || echo 'not set')"
    echo "  Client: $(get_p4_client || echo 'not set')"
    echo "  P4PORT: ${P4PORT:-not set}"
    echo "  P4USER: ${P4USER:-not set}"
    echo "  P4CLIENT: ${P4CLIENT:-not set}"
}

# Export helper functions
export -f check_python3
export -f check_git_p4
export -f validate_p4_environment
export -f extract_p4_metadata
export -f has_p4_metadata
export -f get_p4_depot_path
export -f get_p4_client
export -f show_p4_config
