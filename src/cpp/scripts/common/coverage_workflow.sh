#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/coverage_report.sh"

coverage_collect() {
    coverage_build "$@"
}

coverage_report_compat() {
    if [[ "${1:-}" == "html" || "${1:-}" == "text" ]]; then
        shift || true
    fi
    coverage_report "$@"
}

_coverage_main() {
    local command="${1:-}"

    case "$command" in
        collect)
            shift
            coverage_collect "$@"
            ;;
        merge)
            shift
            coverage_merge "$@"
            ;;
        report)
            shift
            coverage_report_compat "$@"
            ;;
        all)
            shift
            coverage_all "$@"
            ;;
        info)
            coverage_info
            ;;
        help|--help|-h)
            cat <<'EOF'
Usage: coverage_workflow.sh <command> [options]

Compatibility wrapper over coverage_report.sh.

Commands:
  collect [preset]        Alias of canonical build stage
  merge [file]            Alias of canonical merge stage
  report [preset] [bin]   Alias of canonical report stage
  all [preset]            Canonical build + test + merge + report workflow
  info                    Show coverage environment

Canonical path:
  bash src/cpp/scripts/common/coverage_report.sh <command>
EOF
            ;;
        "")
            echo "Error: No command specified" >&2
            echo "Run '$0 help' for usage." >&2
            return 1
            ;;
        *)
            echo "Error: Unknown command: $command" >&2
            echo "Run '$0 help' for usage." >&2
            return 1
            ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    _coverage_main "$@"
fi
