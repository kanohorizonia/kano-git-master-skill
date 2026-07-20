#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CASE_ROOT="${ROOT_DIR}/.kano/tmp/lightweight-launcher-fast-path-$$"

cleanup() {
  rm -rf "$CASE_ROOT"
}
trap cleanup EXIT

mkdir -p \
  "$CASE_ROOT/scripts" \
  "$CASE_ROOT/src/cpp/code/apps" \
  "$CASE_ROOT/src/cpp/code/systems" \
  "$CASE_ROOT/src/cpp/out/bin/macos-ninja-clang/debug" \
  "$CASE_ROOT/fake-bin"

cp "$ROOT_DIR/scripts/kano-git" "$CASE_ROOT/scripts/kano-git"
printf '0.0.0-test\n' > "$CASE_ROOT/VERSION"
printf 'runtime marker\n' > "$CASE_ROOT/src/cpp/code/apps/runtime.txt"

cat > "$CASE_ROOT/src/cpp/out/bin/macos-ninja-clang/debug/kano-git" <<'EOF'
#!/usr/bin/env bash
printf 'native:%s\n' "$*"
EOF
chmod +x "$CASE_ROOT/src/cpp/out/bin/macos-ninja-clang/debug/kano-git"

cat > "$CASE_ROOT/fake-bin/pixi" <<EOF
#!/usr/bin/env bash
printf 'pixi-called\n' >> "$CASE_ROOT/pixi.log"
exit 99
EOF
chmod +x "$CASE_ROOT/fake-bin/pixi"

# The binary is newer than all runtime inputs, so read-only queries must exec it
# before Pixi activation.
sleep 1
touch "$CASE_ROOT/src/cpp/out/bin/macos-ninja-clang/debug/kano-git"
output="$(PATH="$CASE_ROOT/fake-bin:$PATH" bash "$CASE_ROOT/scripts/kano-git" version)"
[[ "$output" == "native:version" ]]
[[ ! -e "$CASE_ROOT/pixi.log" ]]

output="$(PATH="$CASE_ROOT/fake-bin:$PATH" bash "$CASE_ROOT/scripts/kano-git" converge --status --profile 2>"$CASE_ROOT/profile.err")"
[[ "$output" == "native:converge --status --profile" ]]
grep -q '^\[launcher\]\[profile\] fast-path setup=[0-9][0-9]*ms binary=' "$CASE_ROOT/profile.err"
[[ ! -e "$CASE_ROOT/pixi.log" ]]

output="$(PATH="$CASE_ROOT/fake-bin:$PATH" bash "$CASE_ROOT/scripts/kano-git" status --help)"
[[ "$output" == "native:status --help" ]]
[[ ! -e "$CASE_ROOT/pixi.log" ]]

# A newer runtime source makes the binary stale and forces the canonical
# launcher/bootstrap path. The fixture intentionally lacks shared infra, so the
# fallback fails after proving that Pixi resolution was attempted.
sleep 1
touch "$CASE_ROOT/src/cpp/code/apps/runtime.txt"
set +e
PATH="$CASE_ROOT/fake-bin:$PATH" bash "$CASE_ROOT/scripts/kano-git" version \
  >"$CASE_ROOT/fallback.out" 2>"$CASE_ROOT/fallback.err"
status=$?
set -e
[[ "$status" -ne 0 ]]
grep -q 'shared infra bootstrap is missing' "$CASE_ROOT/fallback.err"

echo "PASS: lightweight launcher fast path and stale fallback"
