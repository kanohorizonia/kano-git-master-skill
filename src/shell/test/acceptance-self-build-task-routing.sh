#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CASE_ROOT="${ROOT_DIR}/.kano/tmp/self-build-task-routing-$$"
PIXI_LOG="$CASE_ROOT/pixi.log"

cleanup() {
  rm -rf "$CASE_ROOT"
}
trap cleanup EXIT

mkdir -p \
  "$CASE_ROOT/scripts" \
  "$CASE_ROOT/fake-bin" \
  "$CASE_ROOT/src/cpp/out/release" \
  "$CASE_ROOT/src/cpp/out/debug" \
  "$CASE_ROOT/src/cpp/shared/infra/scripts/lib"

cp "$ROOT_DIR/scripts/kog" "$CASE_ROOT/scripts/kog"
cp "$ROOT_DIR/scripts/kano-git" "$CASE_ROOT/scripts/kano-git"

cat > "$CASE_ROOT/src/cpp/shared/infra/pixi.toml" <<'EOF'
[workspace]
name = "self-build-task-routing-fixture"
EOF

cat > "$CASE_ROOT/src/cpp/shared/infra/scripts/lib/pixi_bootstrap.sh" <<'EOF'
kano_pixi_bootstrap_expose_global_tools() {
  return 0
}

kano_pixi_bootstrap_activate() {
  return 0
}
EOF

cat > "$CASE_ROOT/fake-bin/pixi" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$KOG_TEST_PIXI_LOG"
exit 0
EOF

for tool in cmake ninja; do
  cat > "$CASE_ROOT/fake-bin/$tool" <<'EOF'
#!/usr/bin/env bash
printf '%s fixture\n' "$(basename "$0")"
exit 0
EOF
done

cat > "$CASE_ROOT/fake-bin/uname" <<'EOF'
#!/usr/bin/env bash
if [[ -n "${KOG_TEST_UNAME:-}" ]]; then
  printf '%s\n' "$KOG_TEST_UNAME"
else
  command -p uname "$@"
fi
EOF

cat > "$CASE_ROOT/src/cpp/out/release/kano-git" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cp "$CASE_ROOT/src/cpp/out/release/kano-git" "$CASE_ROOT/src/cpp/out/release/kano-git.exe"
cp "$CASE_ROOT/src/cpp/out/release/kano-git" "$CASE_ROOT/src/cpp/out/debug/kano-git"
cp "$CASE_ROOT/src/cpp/out/release/kano-git" "$CASE_ROOT/src/cpp/out/debug/kano-git.exe"
chmod +x "$CASE_ROOT/fake-bin/"* "$CASE_ROOT/src/cpp/out/release/"* "$CASE_ROOT/src/cpp/out/debug/"*

run_self() {
  PATH="$CASE_ROOT/fake-bin:$PATH" \
    KOG_TEST_PIXI_LOG="$PIXI_LOG" \
    KANO_GIT_BINARY_PATH="" \
    KANO_GIT_BIN="" \
    bash "$CASE_ROOT/scripts/kog" self "$@"
}

: > "$PIXI_LOG"
release_output="$(run_self build)"
[[ "$(tail -n 1 "$PIXI_LOG")" == *" build-release" ]]
[[ "$release_output" == *"Native binary ready: $CASE_ROOT/src/cpp/out/release/kano-git"* ||
   "$release_output" == *"Native binary ready: $CASE_ROOT/src/cpp/out/release/kano-git.exe"* ]]

: > "$PIXI_LOG"
debug_output="$(run_self build --debug)"
[[ "$(tail -n 1 "$PIXI_LOG")" == *" build-debug" ]]
[[ "$debug_output" == *"Debug binary ready: $CASE_ROOT/src/cpp/out/debug/kano-git"* ||
   "$debug_output" == *"Debug binary ready: $CASE_ROOT/src/cpp/out/debug/kano-git.exe"* ]]

: > "$PIXI_LOG"
windows_debug_output="$(KOG_TEST_UNAME=MINGW64_NT-10.0 run_self build --debug)"
[[ "$(tail -n 1 "$PIXI_LOG")" == *" build-debug" ]]
[[ "$windows_debug_output" == *"Debug binary ready: $CASE_ROOT/src/cpp/out/debug/kano-git.exe"* ]]

: > "$PIXI_LOG"
run_self rebuild >/dev/null
[[ "$(tail -n 1 "$PIXI_LOG")" == *" rebuild" ]]

: > "$PIXI_LOG"
run_self build --pgo >/dev/null
[[ "$(tail -n 1 "$PIXI_LOG")" == *" pgo-rebuild" ]]

echo "PASS: self build modes route to Pixi tasks and report the requested configuration"
