#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi
AI_PROVIDER="auto" # auto|codex|copilot|none
COMMIT_MESSAGE=""
DO_PUSH=0
MAX_FILE_SIZE_MB=5
AI_REVIEW=1

usage() {
  cat <<'EOF'
Usage: ai-safe-commit-all-repos.sh [options]

Commit changes across:
  1) root repo
  2) all submodules (recursive)
  3) all nested git repos under this folder (including private repos not in .gitmodules)

Options:
  --ai <auto|codex|copilot|none>  AI provider for commit message (default: auto)
  --ai-review                     Run AI PASS/FAIL safety gate before commit (default: on)
  --no-ai-review                  Disable AI safety gate (only static checks)
  -m, --message <text>            Fixed commit message (skip AI generation)
  -f, --force                     Push after commit with --force-with-lease
  --max-file-size-mb <int>        Block staged files larger than this size (default: 5)
  -h, --help                      Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ai)
      AI_PROVIDER="${2:-}"
      shift 2
      ;;
    --ai-review)
      AI_REVIEW=1
      shift
      ;;
    --no-ai-review)
      AI_REVIEW=0
      shift
      ;;
    -m|--message)
      COMMIT_MESSAGE="${2:-}"
      shift 2
      ;;
    -f|--force)
      DO_PUSH=1
      shift
      ;;
    --max-file-size-mb)
      MAX_FILE_SIZE_MB="${2:-}"
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

if [[ "$AI_PROVIDER" != "auto" && "$AI_PROVIDER" != "codex" && "$AI_PROVIDER" != "copilot" && "$AI_PROVIDER" != "none" ]]; then
  echo "Invalid --ai value: $AI_PROVIDER" >&2
  exit 1
fi

if ! [[ "$MAX_FILE_SIZE_MB" =~ ^[0-9]+$ ]]; then
  echo "--max-file-size-mb must be an integer." >&2
  exit 1
fi

MAX_FILE_SIZE_BYTES=$((MAX_FILE_SIZE_MB * 1024 * 1024))
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

declare -A REPO_SET=()
declare -a REPOS=()

add_repo() {
  local repo="$1"
  if [[ -z "$repo" ]]; then
    return
  fi
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    return
  fi
  repo="$(cd "$repo" && pwd)"
  if [[ -z "${REPO_SET[$repo]+x}" ]]; then
    REPO_SET["$repo"]=1
    REPOS+=("$repo")
  fi
}

ensure_gitignore_entry() {
  local repo="$1"
  local entry="$2"
  local gitignore="$repo/.gitignore"
  if [[ -f "$gitignore" ]] && grep -Fxq "$entry" "$gitignore"; then
    return
  fi
  printf '%s\n' "$entry" >>"$gitignore"
}

maybe_update_gitignore() {
  local repo="$1"
  local changed=0
  local path=""

  while IFS= read -r path; do
    case "$path" in
      .env|.env.*|*.local|*.log|*.tmp|*.swp|*.swo|*.bak|*.pid|*.sqlite|*.sqlite3|*.db|.DS_Store|Thumbs.db)
        ensure_gitignore_entry "$repo" "$path"
        changed=1
        ;;
      node_modules/*|dist/*|build/*|coverage/*|.venv/*|venv/*|__pycache__/*|.pytest_cache/*|.mypy_cache/*|.ruff_cache/*|tmp/*)
        ensure_gitignore_entry "$repo" "${path%%/*}/"
        changed=1
        ;;
    esac
  done < <(git -C "$repo" ls-files --others --exclude-standard)

  local nested_git=""
  local nested_repo=""
  local rel=""
  while IFS= read -r nested_git; do
    nested_repo="$(dirname "$nested_git")"
    if [[ "$nested_repo" == "$repo" ]]; then
      continue
    fi
    rel="${nested_repo#$repo/}"
    if [[ "$rel" == "$nested_repo" ]]; then
      continue
    fi
    if git -C "$repo" check-ignore -q "$rel/"; then
      continue
    fi
    if git -C "$repo" ls-files --error-unmatch "$rel" >/dev/null 2>&1; then
      continue
    fi
    ensure_gitignore_entry "$repo" "$rel/"
    changed=1
  done < <(
    find "$repo" \
      -type d -name .git -prune -print \
      -o -type f -name .git -print 2>/dev/null
  )

  if [[ "$changed" -eq 1 ]]; then
    echo "[$repo] .gitignore updated with common local-only paths."
  fi
}

run_safety_checks() {
  local repo="$1"
  local check_file="$TMP_DIR/check-$(echo "$repo" | sed 's#[^a-zA-Z0-9]#_#g').txt"

  if ! git -C "$repo" diff --cached --check >"$check_file" 2>&1; then
    echo "[$repo] Safety check failed: conflict markers or whitespace issues."
    cat "$check_file"
    return 1
  fi

  local path=""
  local blob_size=0
  while IFS= read -r path; do
    case "$path" in
      *.pem|*.key|*.p12|*.pfx|*.jks|*.kubeconfig|*id_rsa*|*id_dsa*|*id_ed25519*)
        echo "[$repo] Safety check failed: private key-like file staged: $path"
        return 1
        ;;
      .env|.env.*|*secrets*.env|*secret*.env)
        echo "[$repo] Safety check failed: env secret file staged: $path"
        return 1
        ;;
      *auth-profiles.json|*credentials*.json|*token*|*secret*)
        echo "[$repo] Warning: sensitive-looking file staged: $path"
        ;;
    esac

    blob_size="$(git -C "$repo" cat-file -s ":$path" 2>/dev/null || echo 0)"
    if (( blob_size > MAX_FILE_SIZE_BYTES )); then
      echo "[$repo] Safety check failed: file too large (${blob_size} bytes > ${MAX_FILE_SIZE_BYTES}): $path"
      return 1
    fi
  done < <(git -C "$repo" diff --cached --name-only --diff-filter=ACMR)

  if git -C "$repo" diff --cached --text | grep -E -n -i \
    "(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|ghp_[0-9A-Za-z]{36}|github_pat_[0-9A-Za-z_]{80,}|xox[baprs]-[0-9A-Za-z-]{10,}|-----BEGIN (RSA|EC|OPENSSH|DSA|PGP) PRIVATE KEY-----|api[_-]?key[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|token[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|password[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,}|secret[[:space:]]*[:=][[:space:]]*['\"][^'\"]{8,})" \
    >"$check_file"; then
    echo "[$repo] Safety check failed: possible secret detected in staged diff."
    cat "$check_file"
    return 1
  fi

  return 0
}

sanitize_message() {
  local msg="$1"
  msg="$(printf '%s' "$msg" | tr '\r\n' ' ' | sed -E 's/[[:space:]]+/ /g; s/^ //; s/ $//')"
  printf '%s' "$msg"
}

ai_text_from_provider() {
  local prompt="$1"
  local raw=""

  case "$AI_PROVIDER" in
    codex)
      raw="$(ai_message_from_codex "$prompt" || true)"
      ;;
    copilot)
      raw="$(ai_message_from_copilot "$prompt" || true)"
      ;;
    none)
      raw=""
      ;;
    auto)
      raw="$(ai_message_from_codex "$prompt" || true)"
      if [[ -z "$raw" ]]; then
        raw="$(ai_message_from_copilot "$prompt" || true)"
      fi
      ;;
  esac

  printf '%s' "$raw"
}

build_prompt() {
  local repo="$1"
  local stat files
  stat="$(git -C "$repo" diff --cached --shortstat || true)"
  files="$(git -C "$repo" diff --cached --name-status || true)"
  cat <<EOF
Generate one concise Conventional Commit message for this git change.
Rules:
- Output only one line, no quotes, no markdown.
- Use format: type(scope): summary
- Choose type from: feat, fix, refactor, chore, docs, test, ci, build

Repo: $(basename "$repo")
Stats: $stat
Files:
$files
EOF
}

ai_message_from_codex() {
  local prompt="$1"
  if ! command -v codex >/dev/null 2>&1; then
    return 1
  fi
  codex "$prompt" 2>/dev/null | head -n 1
}

ai_message_from_copilot() {
  local prompt="$1"
  if ! command -v gh >/dev/null 2>&1; then
    return 1
  fi
  gh copilot suggest -t shell "$prompt" 2>/dev/null | head -n 1
}

fallback_message() {
  local repo="$1"
  local scope
  scope="$(basename "$repo")"
  printf 'chore(%s): update tracked changes' "$scope"
}

generate_message() {
  local repo="$1"
  local prompt raw msg

  if [[ -n "$COMMIT_MESSAGE" ]]; then
    printf '%s' "$COMMIT_MESSAGE"
    return 0
  fi

  prompt="$(build_prompt "$repo")"
  raw="$(ai_text_from_provider "$prompt")"

  msg="$(sanitize_message "$raw")"
  if [[ -z "$msg" ]]; then
    msg="$(fallback_message "$repo")"
  fi
  printf '%s' "$msg"
}

build_review_prompt() {
  local repo="$1"
  local stat files diff_preview
  stat="$(git -C "$repo" diff --cached --shortstat || true)"
  files="$(git -C "$repo" diff --cached --name-status || true)"
  diff_preview="$(git -C "$repo" diff --cached --unified=0 --text | head -n 300 || true)"
  cat <<EOF
You are a git commit safety reviewer.
Decide if this staged change is safe to commit.
Focus on: secrets, credentials, tokens, private keys, accidental binaries, noisy generated files, risky unintended changes.
Output format (strict):
PASS: <one short reason>
or
FAIL: <one short reason>
Output only one line.

Repo: $(basename "$repo")
Stats: $stat
Files:
$files
Patch preview:
$diff_preview
EOF
}

run_ai_review() {
  local repo="$1"
  local prompt raw first verdict

  if [[ "$AI_REVIEW" -ne 1 ]]; then
    return 0
  fi

  if [[ "$AI_PROVIDER" == "none" ]]; then
    echo "[$repo] AI review enabled but --ai none set; failing closed. Use --no-ai-review to bypass."
    return 1
  fi

  prompt="$(build_review_prompt "$repo")"
  raw="$(ai_text_from_provider "$prompt")"
  if [[ -z "$raw" ]]; then
    echo "[$repo] AI review unavailable (no codex/copilot response), failing closed."
    echo "[$repo] Use --no-ai-review to bypass."
    return 1
  fi
  first="$(printf '%s\n' "$raw" | head -n 1 | tr -d '\r')"
  verdict="$(printf '%s\n' "$first" | sed -E 's/^([Pp][Aa][Ss][Ss]|[Ff][Aa][Ii][Ll]).*$/\1/')"

  if [[ "$verdict" =~ ^[Pp][Aa][Ss][Ss]$ ]]; then
    echo "[$repo] AI review: $first"
    return 0
  fi

  if [[ "$verdict" =~ ^[Ff][Aa][Ii][Ll]$ ]]; then
    echo "[$repo] AI review blocked commit: $first"
    return 1
  fi

  echo "[$repo] AI review returned invalid verdict, failing closed."
  if [[ -n "$first" ]]; then
    echo "[$repo] AI output: $first"
  fi
  return 1
}

commit_repo() {
  local repo="$1"
  local msg branch
  echo
  echo "=== Processing repo: $repo ==="

  local merge_head rebase_merge rebase_apply
  merge_head="$(git -C "$repo" rev-parse --git-path MERGE_HEAD)"
  rebase_merge="$(git -C "$repo" rev-parse --git-path rebase-merge)"
  rebase_apply="$(git -C "$repo" rev-parse --git-path rebase-apply)"
  if [[ -f "$merge_head" || -d "$rebase_merge" || -d "$rebase_apply" ]]; then
    echo "[$repo] Skip: merge/rebase in progress."
    return 0
  fi

  maybe_update_gitignore "$repo"
  git -C "$repo" add -A

  if git -C "$repo" diff --cached --quiet; then
    echo "[$repo] No staged changes, skip."
    return 0
  fi

  if ! run_safety_checks "$repo"; then
    echo "[$repo] Commit aborted due to safety checks."
    return 1
  fi

  if ! run_ai_review "$repo"; then
    echo "[$repo] Commit aborted due to AI review."
    return 1
  fi

  msg="$(generate_message "$repo")"
  if [[ -z "$msg" ]]; then
    echo "[$repo] Failed to generate commit message."
    return 1
  fi

  echo "[$repo] Commit message: $msg"
  git -C "$repo" commit -m "$msg"

  if [[ "$DO_PUSH" -eq 1 ]]; then
    branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD || true)"
    if [[ -z "$branch" ]]; then
      echo "[$repo] Detached HEAD; skip push."
    else
      echo "[$repo] Pushing branch '$branch' with --force-with-lease..."
      git -C "$repo" push --force-with-lease origin "$branch"
    fi
  fi
}

add_repo "$ROOT"

while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  add_repo "$ROOT/$line"
done < <(git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}')

while IFS= read -r git_marker; do
  add_repo "$(dirname "$git_marker")"
done < <(
  find "$ROOT" \
    -type d -name .git -prune -print \
    -o -type f -name .git -print 2>/dev/null
)

if [[ "${#REPOS[@]}" -eq 0 ]]; then
  echo "No git repositories found under: $ROOT"
  exit 0
fi

declare -a NON_ROOT=()
for repo in "${REPOS[@]}"; do
  if [[ "$repo" != "$ROOT" ]]; then
    NON_ROOT+=("$repo")
  fi
done

IFS=$'\n' NON_ROOT=($(for r in "${NON_ROOT[@]}"; do printf '%s\n' "$r"; done | awk '{print length, $0}' | sort -rn | cut -d' ' -f2-))
unset IFS

for repo in "${NON_ROOT[@]}"; do
  commit_repo "$repo"
done

commit_repo "$ROOT"

echo
echo "All done."
