#!/usr/bin/env bash
#
# conflict-parser.sh - Git conflict parsing and resolution helpers
#
# Purpose:
#   Parse and extract conflict markers for AI resolution
#

set -euo pipefail

#------------------------------------------------------------------------------
# Conflict Marker Detection
#------------------------------------------------------------------------------

has_conflict_markers() {
  local file="$1"
  grep -q "^<<<<<<< \|^=======$\|^>>>>>>> " "$file" 2>/dev/null
}

extract_conflict_sections() {
  local file="$1"
  local output_file="${2:-}"

  if [[ -z "$output_file" ]]; then
    output_file="$(mktemp)"
  fi

  # Extract all conflict sections with context
  awk '
    /^<<<<<<< / { in_conflict=1; ours_start=NR; print "=== CONFLICT START ===" }
    in_conflict { print }
    /^=======/ { theirs_start=NR }
    /^>>>>>>> / { in_conflict=0; print "=== CONFLICT END ===" }
  ' "$file" > "$output_file"

  echo "$output_file"
}

parse_conflict_markers() {
  local file="$1"
  local conflict_num=0

  awk '
    BEGIN { conflict_num=0 }
    /^<<<<<<< / {
      conflict_num++
      ours_label=$2
      ours_content=""
      theirs_content=""
      in_ours=1
      in_theirs=0
      next
    }
    /^=======/ {
      in_ours=0
      in_theirs=1
      next
    }
    /^>>>>>>> / {
      theirs_label=$2
      print "CONFLICT_" conflict_num "_OURS_LABEL=" ours_label
      print "CONFLICT_" conflict_num "_THEIRS_LABEL=" theirs_label
      print "CONFLICT_" conflict_num "_OURS_CONTENT<<EOF"
      print ours_content
      print "EOF"
      print "CONFLICT_" conflict_num "_THEIRS_CONTENT<<EOF"
      print theirs_content
      print "EOF"
      print ""
      in_theirs=0
      next
    }
    in_ours { ours_content = ours_content $0 "\n" }
    in_theirs { theirs_content = theirs_content $0 "\n" }
  ' "$file"
}

count_conflict_markers() {
  local file="$1"
  grep -c "^<<<<<<< " "$file" 2>/dev/null || echo "0"
}

#------------------------------------------------------------------------------
# Conflict Context Extraction
#------------------------------------------------------------------------------

get_conflict_context() {
  local file="$1"
  local lines_before="${2:-5}"
  local lines_after="${3:-5}"

  # Get file content with line numbers
  cat -n "$file" | awk -v before="$lines_before" -v after="$lines_after" '
    /^[[:space:]]*[0-9]+[[:space:]]*<<<<<<< / {
      conflict_line = $1
      # Print context before
      for (i = conflict_line - before; i < conflict_line; i++) {
        if (i in lines) print lines[i]
      }
    }
    { lines[$1] = $0 }
    /^[[:space:]]*[0-9]+[[:space:]]*>>>>>>> / {
      end_line = $1
      # Print conflict and context after
      for (i = conflict_line; i <= end_line + after; i++) {
        if (i in lines) print lines[i]
      }
      print "---"
    }
  '
}

#------------------------------------------------------------------------------
# AI Resolution Helpers
#------------------------------------------------------------------------------

build_conflict_prompt() {
  local file="$1"
  local repo="${2:-.}"

  local file_type="${file##*.}"
  local conflict_count
  conflict_count="$(count_conflict_markers "$file")"

  cat <<EOF
You are a Git conflict resolution expert.
Analyze the following conflict and provide a resolution.

File: $file
Type: $file_type
Conflicts: $conflict_count

Context:
$(get_conflict_context "$file" 3 3)

Instructions:
1. Analyze both versions (OURS vs THEIRS)
2. Understand the intent of each change
3. Provide a merged version that preserves both intents when possible
4. If changes are incompatible, choose the better approach and explain why
5. Output only the resolved content without conflict markers

Output format:
RESOLUTION:
<resolved content here>

EXPLANATION:
<brief explanation of resolution strategy>
EOF
}

apply_resolution() {
  local file="$1"
  local resolution="$2"
  local backup="${file}.conflict-backup"

  # Backup original
  cp "$file" "$backup"

  # Apply resolution
  echo "$resolution" > "$file"

  echo "$backup"
}

#------------------------------------------------------------------------------
# Conflict Statistics
#------------------------------------------------------------------------------

get_conflict_stats() {
  local repo="${1:-.}"

  local total_files=0
  local total_conflicts=0

  while IFS= read -r file; do
    ((total_files++)) || true
    local count
    count="$(count_conflict_markers "$repo/$file")"
    ((total_conflicts += count)) || true
  done < <(git -C "$repo" diff --name-only --diff-filter=U 2>/dev/null || true)

  cat <<EOF
Total conflicted files: $total_files
Total conflict markers: $total_conflicts
Average conflicts per file: $((total_conflicts / (total_files > 0 ? total_files : 1)))
EOF
}
