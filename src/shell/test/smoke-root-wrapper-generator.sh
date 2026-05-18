#!/usr/bin/env bash
#
# smoke-root-wrapper-generator.sh
# Verifies that root wrapper template generation works for all supported profiles.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
GENERATOR="$REPO_ROOT/src/shell/core/gen-root-wrappers.sh"

if [[ ! -x "$GENERATOR" ]]; then
  echo "Error: generator is not executable: $GENERATOR" >&2
  exit 1
fi

tmp_root="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_root" 2>/dev/null || true
}
trap cleanup EXIT

profiles=(standalone oss repo-passive-mode repo-passive-mode-with-ai)
for profile in "${profiles[@]}"; do
  target="$tmp_root/$profile"
  mkdir -p "$target"
  "$GENERATOR" --profile "$profile" --target "$target" --force >/dev/null

  test -x "$target/kog"
  test -x "$target/kog-sync.sh"
  test -x "$target/kog-refresh-wrappers.sh"
  test -f "$target/.kog-wrapper-profile"
  [[ "$(cat "$target/.kog-wrapper-profile")" == "$profile" ]]

  case "$profile" in
    oss)
      test -x "$target/kog-sync-upstream-stable-dev.sh"
      ;;
    repo-passive-mode)
      if [[ -e "$target/kog-commit-with-ai-review.sh" || -e "$target/kog-commit-push-with-ai-review.sh" ]]; then
        echo "Error: passive profile should exclude with-ai-review wrappers" >&2
        exit 1
      fi
      ;;
    repo-passive-mode-with-ai)
      test -x "$target/kog-commit-with-ai-review.sh"
      test -x "$target/kog-commit-push-with-ai-review.sh"
      ;;
  esac

done

"$GENERATOR" --profile standalone --target "$tmp_root/dry-run" --dry-run >/dev/null

bash -n "$GENERATOR"

echo "PASS: root wrapper generator smoke test"
