#!/usr/bin/env bash
#
# smoke-export-manifest.sh
# Validates the export-manifest.json produced by `kog export --single`.
#
# Usage (run from workspace root after kog export --single):
#   src/shell/test/smoke-export-manifest.sh [manifest-path]
#
# Default manifest path: .kano/tmp/export-manifest.json
#
# Checks:
#   A. Manifest file exists
#   B. Manifest is valid JSON
#   C. Required fields are present: schemaVersion, exportMode, singleArchive,
#      path, archiveFile, archives[0].path, archives[0].archiveFile,
#      sha256, sizeBytes, platform
#   D. exportMode == "single" and singleArchive == true
#   E. platform is one of: windows / linux / mac
#   F. archiveFile / path use forward slashes (no backslashes)
#   G. The archive file referenced by the manifest exists
#   H. sha256 matches the actual archive file hash

set -euo pipefail

MANIFEST="${1:-}"

# If no argument given, find the manifest by glob under .kano/tmp/
if [[ -z "$MANIFEST" ]]; then
  MANIFEST=$(ls .kano/tmp/*_rev*.export-manifest.json 2>/dev/null | sort | tail -1 || true)
fi

if [[ -z "$MANIFEST" ]]; then
  fail "no export-manifest.json found under .kano/tmp/ (expected *_revNNN.export-manifest.json)"
fi

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

# A. Manifest exists
[[ -f "$MANIFEST" ]] || fail "manifest not found: $MANIFEST"
pass "manifest exists: $MANIFEST"

# B. Valid JSON
python3 -m json.tool "$MANIFEST" > /dev/null 2>&1 || fail "manifest is not valid JSON"
pass "manifest is valid JSON"

# Use python3 for all subsequent checks to avoid jq dependency
python3 - "$MANIFEST" <<'PYEOF'
import json, sys, os, subprocess

manifest_path = sys.argv[1]
with open(manifest_path) as f:
    m = json.load(f)

errors = []

# C. Required fields
required_keys = [
    "schemaVersion", "exportMode", "singleArchive",
    "path", "archiveFile", "sha256", "sizeBytes", "platform", "archives",
]
for key in required_keys:
    if key not in m:
        errors.append(f"missing required key: {key}")

if "archives" in m and len(m["archives"]) > 0:
    arc = m["archives"][0]
    for key in ("path", "archiveFile"):
        if key not in arc:
            errors.append(f"missing archives[0].{key}")
else:
    errors.append("archives array is empty or missing")

# D. exportMode and singleArchive
if m.get("exportMode") != "single":
    errors.append(f"exportMode must be 'single', got: {m.get('exportMode')!r}")
if m.get("singleArchive") is not True:
    errors.append(f"singleArchive must be true, got: {m.get('singleArchive')!r}")

# E. platform
valid_platforms = {"windows", "linux", "mac"}
if m.get("platform") not in valid_platforms:
    errors.append(f"platform must be one of {valid_platforms}, got: {m.get('platform')!r}")

# F. Forward slashes only
for key in ("archiveFile", "path"):
    val = m.get(key, "")
    if "\\" in val:
        errors.append(f"{key} contains backslash: {val!r}")
if "archives" in m and len(m["archives"]) > 0:
    for key in ("archiveFile", "path"):
        val = m["archives"][0].get(key, "")
        if "\\" in val:
            errors.append(f"archives[0].{key} contains backslash: {val!r}")

# G. Archive file exists
archive_path = m.get("archiveFile", "")
if archive_path and not os.path.exists(archive_path):
    errors.append(f"archive file not found: {archive_path}")

# H. sha256 matches
if archive_path and os.path.exists(archive_path) and m.get("sha256"):
    try:
        actual = subprocess.check_output(["sha256sum", archive_path]).decode().split()[0]
        if actual.lower() != m["sha256"].lower():
            errors.append(f"sha256 mismatch: manifest={m['sha256']} actual={actual}")
    except Exception as e:
        # sha256sum not available — skip hash check
        print(f"  (sha256 check skipped: {e})")

if errors:
    for e in errors:
        print(f"FAIL: {e}", file=sys.stderr)
    sys.exit(1)

print("PASS: all manifest checks passed")
PYEOF
