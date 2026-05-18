#!/usr/bin/env bash
#
# audit-public-doc-script-refs.sh
# Checks current public docs for stale root-shell workflow references.
# Historical references are allowed only when the same line explicitly marks
# them as retired, historical, legacy, stale, or not current.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

strict_files=(
  "README.md"
  "SKILL.md"
  "docs/README.md"
  "docs/guides/current-command-surface.md"
  "docs/guides/cpa-commit-plan-workflow.md"
  "docs/repo-hygiene.md"
  "docs/development/testing.md"
)

# Root shell surfaces that are no longer the current public command contract.
# The canonical current surface is ./scripts/kog and ./scripts/kano-git.
stale_patterns=(
  '(^|[^[:alnum:]_./-])(\.\./|\./)?scripts/(core|internal|submodules|self|stages|workflows|workspace|repo-management|branch-operations|commit-tools|test|category|vcs-bridges|mono-repo|subtree)/[^[:space:]`"'\''<>)]+'
  '(^|[^[:alnum:]_./-])(\.\./|\./)?scripts/(kog-installer|kano-git-installer)([^[:alnum:]_./-]|$)'
  '(^|[^[:alnum:]_./-])(\.\./|\./)?scripts/(update-repo|smart-clone|init-empty-repo|rebase-to-upstream-latest|discover-repos|update-workspace-repos|foreach-repo)\.sh([^[:alnum:]_./-]|$)'
  '(^|[^[:alnum:]_./-])(\.\./|\./)?scripts/smart-[^[:space:]`"'\''<>)]+'
)

# Lines containing these words are allowed to mention stale paths as warnings,
# migration notes, or historical references.
allowed_context_re='retired|historical|legacy|stale|not current|not part|do not|do not use|do not treat|unless|older docs|old root-shell|root-shell workflow examples|historical docs'

failures=0

for file in "${strict_files[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "Error: strict public doc is missing: $file" >&2
    failures=$((failures + 1))
    continue
  fi

  line_no=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    line_no=$((line_no + 1))
    lower_line="$(printf '%s' "$line" | tr '[:upper:]' '[:lower:]')"
    if [[ "$lower_line" =~ $allowed_context_re ]]; then
      continue
    fi

    for pattern in "${stale_patterns[@]}"; do
      if [[ "$line" =~ $pattern ]]; then
        echo "Error: stale root script reference in $file:$line_no" >&2
        echo "  $line" >&2
        failures=$((failures + 1))
        break
      fi
    done
  done < "$file"
done

if (( failures > 0 )); then
  echo "FAIL: public docs contain stale root-shell script references ($failures issue(s))." >&2
  exit 1
fi

echo "PASS: public docs script reference audit"
