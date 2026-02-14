#!/usr/bin/env bash
#
# ai-providers.sh - AI provider abstraction layer for smart-commit
#
# Purpose:
#   Unified interface for multiple AI coding assistants:
#   - OpenCode (native CLI)
#   - Codex (OpenAI)
#   - Copilot (GitHub)
#
# Functions:
#   - fetch_*_models()        - Get available models from each provider
#   - list_available_models() - Unified model listing with cache
#   - clear_cache()           - Clear model cache (all or specific provider)
#   - ai_generate_message()   - Generate commit message via provider
#
# Cache:
#   - Models cached in ~/.cache/kano-git-master-skill/models/
#   - TTL: 24 hours
#   - Use --clear-cache flag to refresh
#

set -euo pipefail

# Cache configuration
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/kano-git-master-skill/models"
CACHE_TTL_SECONDS=$((24 * 60 * 60))  # 24 hours

#------------------------------------------------------------------------------
# Cache Management
#------------------------------------------------------------------------------

ensure_cache_dir() {
  mkdir -p "$CACHE_DIR"
}

get_cache_file() {
  local provider="$1"
  printf '%s/%s.txt' "$CACHE_DIR" "$provider"
}

is_cache_valid() {
  local cache_file="$1"

  if [[ ! -f "$cache_file" ]]; then
    return 1
  fi

  local cache_age
  cache_age=$(( $(date +%s) - $(stat -c %Y "$cache_file" 2>/dev/null || stat -f %m "$cache_file" 2>/dev/null || echo 0) ))

  if (( cache_age > CACHE_TTL_SECONDS )); then
    return 1
  fi

  return 0
}

write_cache() {
  local provider="$1"
  local content="$2"
  local cache_file

  cache_file="$(get_cache_file "$provider")"
  printf '%s\n' "$content" > "$cache_file"
}

read_cache() {
  local provider="$1"
  local cache_file

  cache_file="$(get_cache_file "$provider")"
  if [[ -f "$cache_file" ]]; then
    cat "$cache_file"
  fi
}

clear_cache() {
  local provider="${1:-}"

  if [[ -n "$provider" ]]; then
    local cache_file
    cache_file="$(get_cache_file "$provider")"
    if [[ -f "$cache_file" ]]; then
      rm -f "$cache_file"
      echo "Cleared cache for: $provider"
    fi
  else
    if [[ -d "$CACHE_DIR" ]]; then
      rm -rf "$CACHE_DIR"
      echo "Cleared all model caches"
    fi
  fi
}

#------------------------------------------------------------------------------
# Provider Detection
#------------------------------------------------------------------------------

have_cmd() {
  command -v "$1" >/dev/null 2>&1
}

detect_opencode() {
  have_cmd opencode
}

detect_codex() {
  have_cmd codex
}

detect_copilot() {
  if have_cmd copilot; then
    return 0
  fi

  if have_cmd gh && gh copilot --version >/dev/null 2>&1; then
    return 0
  fi

  return 1
}

#------------------------------------------------------------------------------
# OpenCode Provider
#------------------------------------------------------------------------------

fetch_opencode_models() {
  if ! detect_opencode; then
    echo "ERROR: opencode command not found" >&2
    return 1
  fi

  # Use native opencode models command
  opencode models 2>/dev/null | grep -v '^$' || true
}

#------------------------------------------------------------------------------
# Codex Provider (OpenAI)
#------------------------------------------------------------------------------

fetch_codex_models() {
  if ! detect_codex; then
    echo "ERROR: codex command not found" >&2
    return 1
  fi

  # Try CLI command first
  if codex models 2>/dev/null | grep -v '^$'; then
    return 0
  fi

  # Fallback: scrape from official docs
  local url="https://developers.openai.com/codex/models"
  local models

  models=$(curl -sL "$url" 2>/dev/null | \
    grep -oE 'gpt-[0-9](\.[0-9])?(-[a-z]+)*' | \
    sort -u || true)

  if [[ -n "$models" ]]; then
    printf '%s\n' "$models"
    return 0
  fi

  # Hardcoded fallback (last resort)
  cat <<'EOF'
gpt-5.3-codex
gpt-5.2-codex
gpt-5.1-codex
gpt-5.1-codex-mini
gpt-5-codex
gpt-5-mini
gpt-4-turbo
o1-preview
o3
EOF
}

#------------------------------------------------------------------------------
# Copilot Provider (GitHub)
#------------------------------------------------------------------------------

fetch_copilot_models() {
  if ! detect_copilot; then
    echo "ERROR: copilot command not found" >&2
    return 1
  fi

  # Try CLI help output
  if have_cmd copilot; then
    if copilot --help 2>&1 | grep -oE '(gpt|claude|gemini)-[a-z0-9.-]+' | sort -u; then
      return 0
    fi
  fi

  # Scrape from GitHub docs
  local url="https://docs.github.com/en/copilot/reference/ai-models/supported-models"
  local models

  models=$(curl -sL "$url" 2>/dev/null | \
    grep -oE '(GPT|Claude|Gemini|Grok)-[A-Za-z0-9.-]+' | \
    tr '[:upper:]' '[:lower:]' | \
    sort -u || true)

  if [[ -n "$models" ]]; then
    printf '%s\n' "$models"
    return 0
  fi

  # Hardcoded fallback
  cat <<'EOF'
gpt-5.1-codex
gpt-5.1-codex-mini
gpt-5-codex
gpt-5-mini
gpt-4o-mini
claude-4.5-sonnet
claude-4-sonnet
gemini-3-pro
gemini-3-flash
EOF
}

#------------------------------------------------------------------------------
# Unified Interface
#------------------------------------------------------------------------------

list_available_models() {
  local provider="${1:-}"
  local force_refresh="${2:-false}"

  ensure_cache_dir

  # If provider specified, list only that provider
  if [[ -n "$provider" ]]; then
    local cache_file
    cache_file="$(get_cache_file "$provider")"

    if [[ "$force_refresh" == "true" ]] || ! is_cache_valid "$cache_file"; then
      local models
      case "$provider" in
        opencode)
          models="$(fetch_opencode_models 2>/dev/null || true)"
          ;;
        codex)
          models="$(fetch_codex_models 2>/dev/null || true)"
          ;;
        copilot)
          models="$(fetch_copilot_models 2>/dev/null || true)"
          ;;
        *)
          echo "ERROR: Unknown provider: $provider" >&2
          return 1
          ;;
      esac

      if [[ -n "$models" ]]; then
        write_cache "$provider" "$models"
        printf '%s\n' "$models"
      else
        echo "ERROR: Failed to fetch models for $provider" >&2
        return 1
      fi
    else
      read_cache "$provider"
    fi
    return 0
  fi

  # List all available providers
  local providers=()
  detect_opencode && providers+=(opencode)
  detect_codex && providers+=(codex)
  detect_copilot && providers+=(copilot)

  if [[ "${#providers[@]}" -eq 0 ]]; then
    echo "ERROR: No AI providers detected" >&2
    echo "Install one of: opencode, codex, copilot" >&2
    return 1
  fi

  # List models from all providers
  for prov in "${providers[@]}"; do
    echo "=== $prov ==="
    list_available_models "$prov" "$force_refresh" || true
    echo ""
  done
}

#------------------------------------------------------------------------------
# Message Generation
#------------------------------------------------------------------------------

ai_generate_message() {
  local provider="$1"
  local model="$2"
  local prompt="$3"

  case "$provider" in
    opencode)
      if ! detect_opencode; then
        echo "ERROR: opencode not available" >&2
        return 1
      fi
      opencode run --model "$model" "$prompt" || true
      ;;

    codex)
      if ! detect_codex; then
        echo "ERROR: codex not available" >&2
        return 1
      fi
      codex -q --model "$model" "$prompt" || true
      ;;

    copilot)
      if ! detect_copilot; then
        echo "ERROR: copilot not available" >&2
        return 1
      fi
      if have_cmd copilot; then
        if [[ -n "$model" && "$model" != "auto" ]]; then
          copilot -s -p "$prompt" --model "$model" --no-color --stream off --no-ask-user || true
        else
          copilot -s -p "$prompt" --no-color --stream off --no-ask-user || true
        fi
      else
        if [[ -n "$model" && "$model" != "auto" ]]; then
          gh copilot -- -s -p "$prompt" --model "$model" --no-color --stream off --no-ask-user || true
        else
          gh copilot -- -s -p "$prompt" --no-color --stream off --no-ask-user || true
        fi
      fi
      ;;

    *)
      echo "ERROR: Unknown provider: $provider" >&2
      return 1
      ;;
  esac
}

#------------------------------------------------------------------------------
# Output Normalization Helpers
#------------------------------------------------------------------------------

ai_first_line() {
  # Extract first non-empty line from stdin.
  awk 'NF{print; exit}'
}

ai_strip_copilot_markup() {
  # Copilot CLI may emit bullets and HTML-like wrappers (e.g. "● <p>PASS</p>").
  # Normalize to plain text while keeping content.
  sed -E \
    -e 's/^●[[:space:]]*//' \
    -e 's#</?p>##g' \
    -e 's#</?code>##g' \
    -e 's#</?pre>##g' \
    -e 's#</?blockquote>##g'
}

ai_generate_message_first_line() {
  # Compatibility layer for scripts expecting a single-line response.
  # Returns the first non-empty line after provider-specific normalization.
  local provider="$1"
  local model="$2"
  local prompt="$3"

  case "$provider" in
    copilot)
      ai_generate_message "$provider" "$model" "$prompt" 2>/dev/null | ai_strip_copilot_markup | ai_first_line || true
      ;;
    *)
      ai_generate_message "$provider" "$model" "$prompt" 2>/dev/null | ai_first_line || true
      ;;
  esac
}

#------------------------------------------------------------------------------
# Auto Provider Selection with Fallback
#------------------------------------------------------------------------------

try_providers_in_order() {
  local prompt="$1"
  local providers=("copilot" "codex" "opencode")
  local default_models=("gpt-5-mini" "gpt-5.3-codex" "auto")

  for i in "${!providers[@]}"; do
    local provider="${providers[$i]}"
    local model="${default_models[$i]}"

    # Check if provider is available
    case "$provider" in
      copilot)
        if ! detect_copilot; then
          echo "[$provider] Not available, trying next..." >&2
          continue
        fi
        ;;
      codex)
        if ! detect_codex; then
          echo "[$provider] Not available, trying next..." >&2
          continue
        fi
        ;;
      opencode)
        if ! detect_opencode; then
          echo "[$provider] Not available, trying next..." >&2
          continue
        fi
        ;;
    esac

    echo "[$provider] Attempting with model: $model" >&2

    # Try to generate message
    local result
    result="$(ai_generate_message_first_line "$provider" "$model" "$prompt" || true)"

    if [[ -n "$result" ]]; then
      echo "[$provider] Success!" >&2
      echo "$result"
      return 0
    else
      echo "[$provider] Failed, trying next..." >&2
    fi
  done

  # All providers failed
  echo "[fallback] All AI providers failed" >&2
  return 1
}

generate_fallback_message() {
  local repo="${1:-.}"
  local scope
  scope="$(basename "$repo")"

  # Simple fallback: use git diff stats
  local files_changed
  files_changed="$(git -C "$repo" diff --cached --name-only 2>/dev/null | wc -l)"

  if [[ "$files_changed" -eq 1 ]]; then
    local file
    file="$(git -C "$repo" diff --cached --name-only 2>/dev/null)"
    printf 'chore(%s): update %s' "$scope" "$(basename "$file")"
  else
    printf 'chore(%s): update %d files' "$scope" "$files_changed"
  fi
}
