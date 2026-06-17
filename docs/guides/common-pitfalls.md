# Common Pitfalls and Solutions

A collection of common issues encountered when working with Git automation scripts, especially in non-interactive environments and cross-platform scenarios.

## Table of Contents

1. [Non-TTY Rebase/Editor Issues](#non-tty-rebaseeditor-issues)
2. [pipefail + head SIGPIPE (Exit Code 141)](#pipefail--head-sigpipe-exit-code-141)
3. [Zsh vs Bash Differences](#zsh-vs-bash-differences)
4. [Windows Path Portability](#windows-path-portability)
5. [Repository Hygiene](#repository-hygiene)

---

## Non-TTY Rebase/Editor Issues

### Problem

When running `git rebase --continue` or similar commands in non-interactive environments (like CI/CD, agent execution, or background processes), Git may try to open an interactive editor (Vim, nano, etc.), causing the process to hang indefinitely.

**Symptoms:**
- Script hangs when running `git rebase --continue`
- Process waits for user input that never comes
- No error message, just infinite wait

### Solution

Set `GIT_EDITOR` to a no-op command to prevent Git from opening an interactive editor:

```bash
# Option 1: Environment variable (recommended for scripts)
export GIT_EDITOR=:
git rebase --continue

# Option 2: Inline config (one-off commands)
git -c core.editor=true rebase --continue

# Option 3: Global config (for automation environments)
git config --global core.editor true
```

### Best Practices

```bash
# In automation scripts, always set GIT_EDITOR
set -euo pipefail

# Ensure non-interactive mode
export GIT_EDITOR=:
export GIT_SEQUENCE_EDITOR=:

# For rebase operations
git rebase --continue || {
  echo "Rebase failed, check for conflicts"
  exit 1
}
```

### Related Commands

- `git commit --amend` - May also trigger editor
- `git merge` - Can open editor for merge commit messages
- `git tag -a` - Opens editor for annotated tags

---

## pipefail + head SIGPIPE (Exit Code 141)

### Problem

When using `set -o pipefail` in bash scripts, piping output to `head` often results in exit code 141 (SIGPIPE), which looks like a failure but is actually normal behavior.

**Why it happens:**
- `head` closes the pipe after reading N lines
- The upstream command receives SIGPIPE when trying to write more
- With `pipefail`, the pipeline returns the non-zero exit code (141)

### Example

```bash
set -euo pipefail

# This will fail with exit code 141
git log --oneline | head -n 10

# Error: command exited with code 141
```

### Solutions

#### Solution 1: Avoid head in critical paths

```bash
# Instead of:
models=$(fetch_models | head -n 1)

# Use:
models=$(fetch_models)
first_model=$(echo "$models" | head -n 1)
```

#### Solution 2: Disable pipefail for specific commands

```bash
set -euo pipefail

# Temporarily disable pipefail
set +o pipefail
result=$(long_command | head -n 10)
set -o pipefail
```

#### Solution 3: Write to file first

```bash
# Write full output to temp file
fetch_models > "$TMP_FILE"

# Then safely use head
head -n 10 "$TMP_FILE"
```

#### Solution 4: Ignore SIGPIPE specifically

```bash
# Only for bash 4.4+
set -euo pipefail

# This won't fail on SIGPIPE
result=$(fetch_models | head -n 1 || test $? -eq 141)
```

### Best Practices

```bash
# For model fetching in ai-providers.sh
fetch_opencode_models() {
  # Don't use head here - return all models
  opencode models 2>/dev/null | grep -v '^$' || true
}

# Let the caller decide if they want to limit output
ai_generate_message() {
  local response
  response=$(opencode run --model "$model" "$prompt" 2>/dev/null || true)
  
  # Safe to use head here - not in a pipefail context
  printf '%s' "$response" | head -n 1
}
```

---

## Zsh vs Bash Differences

### Problem

Scripts written for bash may fail or behave differently in zsh due to syntax and behavior differences.

### Key Differences

#### 1. mapfile / readarray

**Bash:**
```bash
# mapfile is bash-only
mapfile -t lines < file.txt
```

**Zsh:**
```zsh
# Use array assignment with command substitution
lines=("${(@f)$(cat file.txt)}")

# Or use a while loop
while IFS= read -r line; do
  lines+=("$line")
done < file.txt
```

#### 2. String Splitting

**Bash:**
```bash
# Bash splits on whitespace by default
files="file1.txt file2.txt file3.txt"
for f in $files; do
  echo "$f"
done
```

**Zsh:**
```zsh
# Zsh does NOT split by default
files="file1.txt file2.txt file3.txt"

# Need explicit splitting
for f in ${=files}; do
  echo "$f"
done

# Or use array
files=(file1.txt file2.txt file3.txt)
for f in "${files[@]}"; do
  echo "$f"
done
```

#### 3. Multiline String to Array

**Bash:**
```bash
# Bash: mapfile handles newlines
mapfile -t items <<EOF
item1
item2
item3
EOF
```

**Zsh:**
```zsh
# Zsh: Use (f) flag for line splitting
items=("${(@f)$(cat <<EOF
item1
item2
item3
EOF
)}")

# Or simpler:
items=(
  item1
  item2
  item3
)
```

### Portable Script Template

```bash
#!/usr/bin/env bash
#
# Use bash shebang for portability
# Avoid zsh-specific features

set -euo pipefail

# Detect shell (if needed)
if [ -n "${ZSH_VERSION:-}" ]; then
  # Running in zsh
  setopt SH_WORD_SPLIT  # Enable bash-like word splitting
fi

# Use portable constructs
declare -a items=()

# Portable way to read lines
while IFS= read -r line; do
  items+=("$line")
done < <(command_that_outputs_lines)

# Portable array iteration
for item in "${items[@]}"; do
  echo "$item"
done
```

---

## Windows Path Portability

### Problem

Windows filesystem doesn't allow colons (`:`) in filenames, but ISO 8601 timestamps use colons in time components (e.g., `2026-02-12T05:21:54Z`).

**Symptoms:**
- Folder creation fails on Windows
- `mkdir: cannot create directory 'T05:21:54Z': Invalid argument`
- Cross-platform scripts break on Windows

### Solution: Windows-Safe Timestamps

Replace colons with hyphens in time components:

```bash
# Bad (fails on Windows)
timestamp=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
# Result: 2026-02-12T05:21:54Z (contains colons)

# Good (works everywhere)
timestamp=$(date -u +"%Y-%m-%dT%H-%M-%SZ")
# Result: 2026-02-12T05-21-54Z (no colons)
```

### Migration: Updating Existing References

When renaming timestamp folders, you must also update all references:

```bash
# 1. Rename the folder
OLD_TS="2026-02-12T05:21:54Z"
NEW_TS="2026-02-12T05-21-54Z"

mv "path/to/$OLD_TS" "path/to/$NEW_TS"

# 2. Update all references in files
git grep -l "$OLD_TS" | xargs sed -i "s|$OLD_TS|$NEW_TS|g"

# 3. Check for any remaining references
git grep "$OLD_TS"
```

### One-Liner Check for Invalid Paths

```bash
# Check for files/folders with colons (Windows-incompatible)
git ls-files | grep ':' && echo "Found Windows-incompatible paths!" || echo "All paths are Windows-safe"
```

### Best Practices

```bash
# Always use Windows-safe timestamp format
generate_timestamp() {
  date -u +"%Y-%m-%dT%H-%M-%SZ"
}

# Example usage
BACKUP_DIR="backups/$(generate_timestamp)"
mkdir -p "$BACKUP_DIR"

# For logs
LOG_FILE="logs/run-$(generate_timestamp).log"
```

### Validation Script

```bash
#!/usr/bin/env bash
# validate-paths.sh - Check for Windows-incompatible paths

set -euo pipefail

echo "Checking for Windows-incompatible characters in paths..."

# Characters not allowed in Windows filenames: < > : " / \ | ? *
INVALID_CHARS='[<>:"/\\|?*]'

if git ls-files | grep -E "$INVALID_CHARS"; then
  echo "ERROR: Found Windows-incompatible paths above"
  exit 1
else
  echo "✓ All paths are Windows-compatible"
fi
```

---

## Repository Hygiene

### Problem

Local cache files, imported data, and temporary files should never be committed to the repository, but they often get accidentally staged.

### Common Offenders

```
.kano/cache/**              # Local cache data
memory/collected/**         # Imported/collected data
*.local                     # Local config overrides
*.tmp                       # Temporary files
node_modules/               # Dependencies
.venv/                      # Python virtual environments
```

### Solution 1: .gitignore

```gitignore
# .gitignore

# Local caches
.kano/cache/
.cache/
*.cache

# Collected/imported data
memory/collected/
memory/imported/

# Local overrides
*.local
*.local.*

# Temporary files
*.tmp
*.temp
*.swp
*.swo
*.bak

# Build outputs
dist/
build/
*.egg-info/

# Dependencies
node_modules/
.venv/
venv/
```

### Solution 2: Pre-Commit Safety Checks

Add checks to your commit scripts (like `smart-commit.sh`):

```bash
# In run_safety_checks() function

# Check for accidentally staged cache/collected files
local forbidden_patterns=(
  ".kano/cache/"
  "memory/collected/"
  "*.local"
  "*.tmp"
)

for pattern in "${forbidden_patterns[@]}"; do
  if git -C "$repo" diff --cached --name-only | grep -q "$pattern"; then
    echo "[$repo] FAILED: Forbidden file pattern staged: $pattern" >&2
    echo "[$repo] These files should be in .gitignore" >&2
    return 1
  fi
done
```

### Solution 3: Automated .gitignore Updates

The `smart-commit.sh` script includes automatic .gitignore management:

```bash
maybe_update_gitignore() {
  local repo="$1"
  local changed=0

  # Check untracked files for common patterns
  while IFS= read -r path; do
    case "$path" in
      .env|.env.*|*.local|*.log|*.tmp|*.swp|*.bak)
        ensure_gitignore_entry "$repo" "$path"
        changed=1
        ;;
      node_modules/*|dist/*|.venv/*|.cache/*)
        ensure_gitignore_entry "$repo" "${path%%/*}/"
        changed=1
        ;;
    esac
  done < <(git -C "$repo" ls-files --others --exclude-standard)

  if [[ "$changed" -eq 1 ]]; then
    echo "[$repo] .gitignore updated"
  fi
}
```

### Best Practices

1. **Always review staged files before committing:**
   ```bash
   git status
   git diff --cached --name-only
   ```

2. **Use safety checks in automation:**
   ```bash
   # Fail if forbidden patterns are staged
   if git diff --cached --name-only | grep -E '(\.kano/cache/|memory/collected/)'; then
     echo "ERROR: Cache or collected files staged"
     exit 1
   fi
   ```

3. **Document what should be ignored:**
   ```bash
   # Add comments to .gitignore
   # Local caches (never commit)
   .kano/cache/
   
   # Imported data (regenerate from source)
   memory/collected/
   ```

4. **Provide regeneration instructions:**
   ```markdown
   # README.md
   
   ## Regenerating Caches
   
   If you need to regenerate local caches:
   
   ```bash
   rm -rf .kano/cache
   ./scripts/rebuild-cache.sh
   ```
   ```

---

## Quick Reference

### Non-TTY Git Commands

```bash
export GIT_EDITOR=:
export GIT_SEQUENCE_EDITOR=:
git rebase --continue
```

### Safe Piping with head

```bash
# Write to file first
command > "$TMP_FILE"
head -n 10 "$TMP_FILE"
```

### Portable Array Reading

```bash
# Works in both bash and zsh
declare -a items=()
while IFS= read -r line; do
  items+=("$line")
done < <(command)
```

### Windows-Safe Timestamp

```bash
date -u +"%Y-%m-%dT%H-%M-%SZ"
```

### Check for Invalid Paths

```bash
git ls-files | grep ':' && echo "Found invalid paths!"
```

### Validate Staged Files

```bash
git diff --cached --name-only | grep -E '(cache/|collected/)' && echo "ERROR: Cache files staged!"
```

---

## See Also

- [Testing Guide](../development/testing.md) - How to test scripts in different environments
- [Contributing Guide](../development/contributing.md) - Code style and conventions
- `scripts/lib/git-helpers.sh` - Historical reusable Git utility functions; not part of the current launcher surface
