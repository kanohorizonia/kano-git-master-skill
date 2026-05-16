#!/usr/bin/env bash
#
# smoke-release-online-build.sh
# Runs the release archive smoke test plus native self build.
# This intentionally requires network access when CMake FetchContent source modes
# are used, which is the normal developer-build path.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
archive_path="${1:-}"
if [[ -z "$archive_path" ]]; then
  echo "Usage: $0 <archive.tar>" >&2
  exit 2
fi

KOG_RELEASE_SMOKE_BUILD=1 "$script_dir/smoke-release-archive.sh" "$archive_path"
