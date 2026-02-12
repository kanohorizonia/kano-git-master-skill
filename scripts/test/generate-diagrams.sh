#!/usr/bin/env bash
#
# generate-diagrams.sh - Generate diagram images from TEST-DIAGRAMS.md
#
# Purpose:
#   Extract Mermaid diagrams and convert to PNG/SVG images
#
# Requirements:
#   - Node.js and npm
#   - @mermaid-js/mermaid-cli (mmdc)
#
# Installation:
#   npm install -g @mermaid-js/mermaid-cli
#
# Usage:
#   ./generate-diagrams.sh [options]
#
# Options:
#   --format <png|svg>    Output format (default: png)
#   --output <dir>        Output directory (default: diagrams/)
#   -h, --help            Show help
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_FILE="$SCRIPT_DIR/TEST-DIAGRAMS.md"
OUTPUT_DIR="$SCRIPT_DIR/diagrams"
FORMAT="png"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Generate diagram images from TEST-DIAGRAMS.md using Mermaid CLI.

Options:
  --format <png|svg>    Output format (default: png)
  --output <dir>        Output directory (default: diagrams/)
  -h, --help            Show help

Requirements:
  npm install -g @mermaid-js/mermaid-cli

Examples:
  # Generate PNG images
  ./generate-diagrams.sh

  # Generate SVG images
  ./generate-diagrams.sh --format svg

  # Custom output directory
  ./generate-diagrams.sh --output ../docs/images
EOF
}

check_requirements() {
  if ! command -v mmdc >/dev/null 2>&1; then
    echo "ERROR: mermaid-cli (mmdc) not found"
    echo ""
    echo "Install with:"
    echo "  npm install -g @mermaid-js/mermaid-cli"
    echo ""
    echo "Or use online tools:"
    echo "  https://mermaid.live/"
    exit 1
  fi
  
  if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js not found"
    echo "Install Node.js from: https://nodejs.org/"
    exit 1
  fi
}

extract_diagrams() {
  local source="$1"
  local output_dir="$2"
  
  mkdir -p "$output_dir"
  
  echo "Extracting Mermaid diagrams from: $source"
  echo "Output directory: $output_dir"
  echo ""
  
  # Extract diagram blocks
  local in_diagram=0
  local diagram_num=0
  local diagram_name=""
  local diagram_content=""
  
  while IFS= read -r line; do
    if [[ "$line" =~ ^\`\`\`mermaid ]]; then
      in_diagram=1
      diagram_num=$((diagram_num + 1))
      diagram_content=""
      continue
    fi
    
    if [[ "$in_diagram" -eq 1 ]]; then
      if [[ "$line" =~ ^\`\`\`$ ]]; then
        # End of diagram
        in_diagram=0
        
        # Determine diagram name from previous heading
        if [[ -n "$diagram_name" ]]; then
          local filename="${diagram_name// /-}"
          filename="${filename,,}"  # lowercase
          filename="${filename//[^a-z0-9-]/}"  # remove special chars
        else
          local filename="diagram-$diagram_num"
        fi
        
        # Save diagram to temp file
        local temp_file="$output_dir/${filename}.mmd"
        echo "$diagram_content" > "$temp_file"
        
        echo "[$diagram_num] Generating: ${filename}.$FORMAT"
        
        # Generate image
        if mmdc -i "$temp_file" -o "$output_dir/${filename}.$FORMAT" >/dev/null 2>&1; then
          echo "    ✓ Created: $output_dir/${filename}.$FORMAT"
        else
          echo "    ✗ Failed: ${filename}"
        fi
        
        # Clean up temp file
        rm -f "$temp_file"
        
        diagram_name=""
      else
        diagram_content+="$line"$'\n'
      fi
    else
      # Check for heading (diagram name)
      if [[ "$line" =~ ^###[[:space:]](.+)$ ]]; then
        diagram_name="${BASH_REMATCH[1]}"
      fi
    fi
  done < "$source"
  
  echo ""
  echo "Generated $diagram_num diagrams in: $output_dir"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --format)
      FORMAT="${2:-}"
      if [[ "$FORMAT" != "png" && "$FORMAT" != "svg" ]]; then
        echo "ERROR: Invalid format: $FORMAT (must be png or svg)" >&2
        exit 1
      fi
      shift 2
      ;;
    --output)
      OUTPUT_DIR="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Check requirements
check_requirements

# Check source file exists
if [[ ! -f "$SOURCE_FILE" ]]; then
  echo "ERROR: Source file not found: $SOURCE_FILE"
  exit 1
fi

# Extract and generate diagrams
extract_diagrams "$SOURCE_FILE" "$OUTPUT_DIR"

echo ""
echo "Done! View diagrams in: $OUTPUT_DIR"
