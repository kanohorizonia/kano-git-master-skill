#!/usr/bin/env bash
#
# ai-gitignore.sh - AI-driven smart .gitignore pattern analysis and management
#
# Purpose:
#   Analyze untracked files using AI to determine appropriate gitignore patterns.
#   Adds explanatory comments for each pattern based on file analysis.
#
# Features:
#   - AI-powered pattern analysis using multiple provider support
#   - Confidence-based decision making (auto-add vs prompt)
#   - Managed block markers for idempotent execution
#   - Per-pattern explanatory comments
#   - Dry-run mode for preview
#
# Usage:
#   ./ai-gitignore.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --repo <path>              Target repository (default: current repo)
#   --dry-run                  Preview changes without modifying files
#   --no-prompt                Skip confirmation prompts (auto-add all)
#   -h, --help                 Show this help message
#
# Examples:
#   # Analyze and add patterns (with prompts for low-confidence)
#   ./ai-gitignore.sh --provider copilot --model gpt-4o
#
#   # Auto-add all patterns without prompts
#   ./ai-gitignore.sh --provider copilot --model gpt-4o --no-prompt
#
#   # Preview in dry-run mode
#   ./ai-gitignore.sh --provider copilot --model gpt-4o --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load AI providers library
LIB_DIR="$SCRIPT_DIR/lib"
if [[ ! -f "$LIB_DIR/ai-providers.sh" ]]; then
  echo "ERROR: AI provider library not found: $LIB_DIR/ai-providers.sh" >&2
  exit 1
fi
source "$LIB_DIR/ai-providers.sh"

# Configuration
TARGET_REPO="${PWD}"
AI_PROVIDER=""
AI_MODEL=""
DRY_RUN=0
NO_PROMPT=0

# Confidence threshold for auto-adding patterns
CONFIDENCE_THRESHOLD=0.8

# Managed block markers
BLOCK_START="# >>> AI-GITIGNORE"
BLOCK_END="# <<< AI-GITIGNORE"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: ai-gitignore.sh --provider <name> --model <name> [options]

AI-driven smart .gitignore pattern analysis with confidence-based decisions.

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --repo <path>              Target repository (default: current repo)
  --dry-run                  Preview changes without modifying files
  --no-prompt                Skip confirmation prompts (auto-add all)
  -h, --help                 Show this help message

Examples:
  # Analyze and add patterns (with prompts for low-confidence)
  ./ai-gitignore.sh --provider copilot --model gpt-4o

  # Auto-add all patterns without prompts
  ./ai-gitignore.sh --provider copilot --model gpt-4o --no-prompt

  # Preview in dry-run mode
  ./ai-gitignore.sh --provider copilot --model gpt-4o --dry-run

Confidence Tiers:
  HIGH (≥0.8):   Auto-add pattern (explains in comment)
  MEDIUM (0.5-0.8): Show pattern with reason, wait for confirmation
  LOW (<0.5):    Show pattern with explanation, ask for confirmation
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      AI_PROVIDER="${2:-}"
      if [[ -z "$AI_PROVIDER" ]]; then
        echo "ERROR: --provider requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      if [[ -z "$AI_MODEL" ]]; then
        echo "ERROR: --model requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    --repo)
      TARGET_REPO="${2:-}"
      if [[ -z "$TARGET_REPO" ]]; then
        echo "ERROR: --repo requires a path argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-prompt)
      NO_PROMPT=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Validate required parameters
if [[ -z "$AI_PROVIDER" ]]; then
  echo "ERROR: --provider is required" >&2
  usage >&2
  exit 1
fi

if [[ -z "$AI_MODEL" ]]; then
  echo "ERROR: --model is required" >&2
  usage >&2
  exit 1
fi

# Verify repo exists
if [[ ! -d "$TARGET_REPO" ]]; then
  echo "ERROR: Repository not found: $TARGET_REPO" >&2
  exit 1
fi

# Verify it's a git repo
if ! git -C "$TARGET_REPO" rev-parse --git-dir >/dev/null 2>&1; then
  echo "ERROR: Not a git repository: $TARGET_REPO" >&2
  exit 1
fi

#------------------------------------------------------------------------------
# AI Analysis Functions
#------------------------------------------------------------------------------

# Analyze untracked files to determine patterns
analyze_untracked_files() {
  local repo="$1"

  # Get list of untracked files
  git -C "$repo" ls-files --others --exclude-standard 2>/dev/null | head -n 50 || echo ""
}

# Build AI analysis prompt
build_analysis_prompt() {
  local repo="$1"
  local untracked_files="$2"

  cat <<EOF
You are a gitignore expert. Analyze these untracked files and suggest gitignore patterns.

Untracked files (sample):
$untracked_files

For each category of files, suggest ONE .gitignore pattern.
Format your response EXACTLY as:
PATTERN | CONFIDENCE | EXPLANATION

Examples:
dist/ | 0.95 | Compiled build output
*.log | 0.90 | Application log files
.env | 0.99 | Environment variable files

Output ONLY patterns, one per line. No other text.
EOF
}

# Parse AI response into structured format
parse_ai_response() {
  local response="$1"

  # Parse each line as: PATTERN | CONFIDENCE | EXPLANATION
  echo "$response" | while IFS='|' read -r pattern confidence explanation; do
    pattern="${pattern## }"      # Trim leading spaces
    pattern="${pattern%% }"      # Trim trailing spaces
    confidence="${confidence## }"
    confidence="${confidence%% }"
    explanation="${explanation## }"
    explanation="${explanation%% }"

    [[ -z "$pattern" ]] && continue

    # Validate confidence is a number
    if ! [[ "$confidence" =~ ^0?\.?[0-9]+$ ]]; then
      continue
    fi

    # Normalize confidence to 0-1 range
    if (( $(echo "$confidence > 1" | bc -l 2>/dev/null || echo 0) )); then
      confidence=$(echo "scale=2; $confidence / 100" | bc -l 2>/dev/null || echo "0.5")
    fi

    echo "$pattern|$confidence|$explanation"
  done
}

# Decide whether to add pattern based on confidence
should_add_pattern() {
  local confidence="$1"
  local pattern="$2"

  # Convert to comparison-friendly format
  local threshold="$CONFIDENCE_THRESHOLD"

  if (( $(echo "$confidence >= $threshold" | bc -l 2>/dev/null || echo 0) )); then
    return 0  # HIGH confidence - auto-add
  fi

  if [[ "$NO_PROMPT" -eq 1 ]]; then
    return 0  # No-prompt mode - add everything
  fi

  # MEDIUM/LOW confidence - prompt user
  echo ""
  echo "Pattern: $pattern (confidence: $confidence)"
  echo "Add to .gitignore? [y/N]"
  read -r -t 5 response || response="n"

  if [[ "$response" =~ ^[Yy] ]]; then
    return 0
  fi

  return 1
}

# Find or create managed block in .gitignore
find_ai_block() {
  local gitignore="$1"

  if [[ ! -f "$gitignore" ]]; then
    return 1
  fi

  local start_line end_line
  start_line=$(grep -n "^$BLOCK_START$" "$gitignore" 2>/dev/null | cut -d: -f1 | head -n 1 || true)
  end_line=$(grep -n "^$BLOCK_END$" "$gitignore" 2>/dev/null | cut -d: -f1 | head -n 1 || true)

  if [[ -n "$start_line" && -n "$end_line" ]]; then
    echo "$start_line:$end_line"
    return 0
  fi

  return 1
}

# Build AI-generated block with comments
build_ai_block() {
  local patterns_data="$1"

  echo "$BLOCK_START"

  # Track current category for comments
  local last_category=""

  # Process each pattern
  echo "$patterns_data" | while IFS='|' read -r pattern confidence explanation; do
    [[ -z "$pattern" ]] && continue

    # Infer category from pattern (simple heuristic)
    local category=""
    if [[ "$pattern" == *"/"* ]] || [[ "$pattern" == "."* ]]; then
      category="Directories/Config"
    else
      category="Files"
    fi

    # Add category header if changed
    if [[ "$category" != "$last_category" ]]; then
      if [[ -n "$last_category" ]]; then
        echo ""
      fi
      echo "# $category"
      last_category="$category"
    fi

    # Add pattern with explanation comment
    if [[ -n "$explanation" ]]; then
      echo "# $explanation (confidence: $confidence)"
    fi
    echo "$pattern"
  done

  echo "$BLOCK_END"
}

# Update .gitignore with AI-generated patterns
update_gitignore_with_ai() {
  local repo="$1"
  local gitignore="$repo/.gitignore"
  local patterns_data="$2"

  # Create .gitignore if doesn't exist
  if [[ ! -f "$gitignore" ]]; then
    touch "$gitignore"
  fi

  local ai_block
  ai_block=$(build_ai_block "$patterns_data")

  # Find or create block
  local block_info
  if block_info=$(find_ai_block "$gitignore"); then
    # Replace existing block
    local start_line end_line
    IFS=: read -r start_line end_line <<< "$block_info"

    local temp_file
    temp_file=$(mktemp)
    {
      if [[ $((start_line - 1)) -gt 0 ]]; then
        sed -n "1,$((start_line - 1))p" "$gitignore"
      fi

      echo "$ai_block"

      local total_lines
      total_lines=$(wc -l < "$gitignore")
      if [[ $((end_line)) -lt $((total_lines)) ]]; then
        sed -n "$((end_line + 1)),\$p" "$gitignore"
      fi
    } > "$temp_file"

    if [[ "$DRY_RUN" -eq 0 ]]; then
      mv "$temp_file" "$gitignore"
      echo "[$repo] .gitignore updated with AI patterns"
    else
      echo "[$repo] Would update .gitignore with AI patterns"
      rm -f "$temp_file"
    fi
  else
    # Append new block
    if [[ "$DRY_RUN" -eq 0 ]]; then
      echo "" >> "$gitignore"
      echo "$ai_block" >> "$gitignore"
      echo "[$repo] .gitignore updated with AI patterns"
    else
      echo "[$repo] Would append AI patterns to .gitignore"
    fi
  fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

echo "AI .gitignore Pattern Analysis"
echo "=============================="
echo "Target: $TARGET_REPO"
echo "Provider: $AI_PROVIDER"
echo "Model: $AI_MODEL"
[[ "$DRY_RUN" -eq 1 ]] && echo "Mode: DRY-RUN"
[[ "$NO_PROMPT" -eq 1 ]] && echo "Mode: NO-PROMPT (auto-add all)"
echo ""

# Analyze untracked files
echo "Analyzing untracked files..."
untracked_files=$(analyze_untracked_files "$TARGET_REPO")

if [[ -z "$untracked_files" ]]; then
  echo "No untracked files found, nothing to analyze."
  exit 0
fi

# Build prompt and call AI
echo "Querying AI provider ($AI_PROVIDER)..."
prompt=$(build_analysis_prompt "$TARGET_REPO" "$untracked_files")

# Call AI provider
ai_response=$(ai_generate_message "$AI_PROVIDER" "$AI_MODEL" "$prompt" 2>/dev/null || true)

if [[ -z "$ai_response" ]]; then
  echo "ERROR: AI provider returned no response" >&2
  exit 1
fi

# Parse response
echo "Processing AI response..."
patterns_data=$(parse_ai_response "$ai_response")

if [[ -z "$patterns_data" ]]; then
  echo "No valid patterns returned from AI analysis."
  exit 0
fi

# Filter patterns based on confidence (with potential prompts)
filtered_patterns=""
echo "$patterns_data" | while IFS='|' read -r pattern confidence explanation; do
  [[ -z "$pattern" ]] && continue

  if should_add_pattern "$confidence" "$pattern"; then
    echo "$pattern|$confidence|$explanation"
  fi
done | while read -r line; do
  if [[ -n "$line" ]]; then
    filtered_patterns="$filtered_patterns$(echo "$line")
"
  fi
done

# Update .gitignore if patterns found
if [[ -n "$filtered_patterns" ]]; then
  update_gitignore_with_ai "$TARGET_REPO" "$filtered_patterns"
else
  echo "No patterns selected for addition."
fi

exit 0
