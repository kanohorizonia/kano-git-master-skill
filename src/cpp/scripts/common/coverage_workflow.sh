#!/usr/bin/env bash
# =============================================================================
# Coverage Workflow Script
# =============================================================================
# Provides functions for:
#   coverage_collect   - Build with coverage and run tests to collect data
#   coverage_merge    - Merge collected coverage data
#   coverage_report   - Generate HTML/text coverage report
# =============================================================================
set -euo pipefail

# Detect platform and compiler
detect_coverage_environment() {
    local platform
    local compiler_id

    platform="$(uname -s 2>/dev/null || echo "unknown")"
    compiler_id="unknown"

    if [[ -n "${CXX:-}" ]]; then
        if [[ "$CXX" == *"clang"* ]]; then
            compiler_id="Clang"
        elif [[ "$CXX" == *"gcc"* || "$CXX" == "g++"* ]]; then
            compiler_id="GNU"
        elif [[ "$CXX" == *"msvc"* || "$CXX" == "cl"* ]]; then
            compiler_id="MSVC"
        fi
    elif command -v clang++ >/dev/null 2>&1; then
        compiler_id="Clang"
    elif command -v g++ >/dev/null 2>&1; then
        compiler_id="GNU"
    elif command -v cl >/dev/null 2>&1; then
        compiler_id="MSVC"
    fi

    echo "$platform:$compiler_id"
}

# Default coverage directories
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_BUILD_ROOT:-$(pwd)/build}/coverage}"
KOG_COVERAGE_OUTPUT_DIR="${KOG_COVERAGE_OUTPUT_DIR:-$KOG_COVERAGE_ROOT}"
KOG_COVERAGE_REPORT_DIR="${KOG_COVERAGE_REPORT_DIR:-$KOG_COVERAGE_ROOT/report}"

# Unix detect
_is_unix() {
    [[ "$(uname -s 2>/dev/null || echo "unknown")" != "CYGWIN"* && "$(uname -s 2>/dev/null || echo "unknown")" != MINGW* && "$(uname -s 2>/dev/null || echo "unknown")" != MSYS* ]]
}

# Ensure directories exist
coverage_ensure_dirs() {
    mkdir -p "$KOG_COVERAGE_OUTPUT_DIR"
    mkdir -p "$KOG_COVERAGE_REPORT_DIR"
}

# =============================================================================
# coverage_collect - Build with coverage instrumentation and run tests
# =============================================================================
# Usage: coverage_collect [preset-name]
# Example: coverage_collect linux-ninja-gcc-coverage
# =============================================================================
coverage_collect() {
    local preset="${1:-}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_coverage_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[coverage_collect] Starting coverage collection..."
    echo "[coverage_collect] Platform: $platform, Compiler: $compiler_id"

    coverage_ensure_dirs

    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_unix; then
                    preset="linux-ninja-clang-coverage"
                else
                    preset="windows-ninja-clang-coverage"
                fi
                ;;
            GNU)
                if _is_unix; then
                    preset="linux-ninja-gcc-coverage"
                else
                    echo "[coverage_collect] ERROR: GCC coverage on Windows not supported" >&2
                    return 1
                fi
                ;;
            MSVC)
                preset="windows-ninja-msvc-coverage"
                ;;
            *)
                echo "[coverage_collect] ERROR: Unknown compiler: $compiler_id" >&2
                return 1
                ;;
        esac
        echo "[coverage_collect] Auto-selected preset: $preset"
    fi

    echo "[coverage_collect] Cleaning coverage output directory..."
    rm -rf "$KOG_COVERAGE_OUTPUT_DIR"/* 2>/dev/null || true

    echo "[coverage_collect] Configuring with preset: $preset"
    (
        if [[ -n "${KOG_CPP_ROOT:-}" ]]; then
            cd "$KOG_CPP_ROOT"
        fi

        cmake --preset "$preset"
        cmake --build --preset "${preset}" --config Debug
    )

    echo "[coverage_collect] Build complete."
    echo "[coverage_collect] Coverage data output: $KOG_COVERAGE_OUTPUT_DIR"
    echo "[coverage_collect] Run your tests now to generate coverage data."
    echo "[coverage_collect] Then run: coverage_merge"
}

# =============================================================================
# coverage_merge - Merge collected coverage data from multiple runs
# =============================================================================
# Usage: coverage_merge [output-dir]
# Example: coverage_merge                     # uses default
#          coverage_merge /path/to/merged
# =============================================================================
coverage_merge() {
    local output_dir="${1:-$KOG_COVERAGE_OUTPUT_DIR}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_coverage_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[coverage_merge] Starting coverage data merge..."
    echo "[coverage_merge] Platform: $platform, Compiler: $compiler_id"

    coverage_ensure_dirs

    if [[ "$compiler_id" == "Clang" ]]; then
        # Clang: merge .profraw files using llvm-profdata
        if ! command -v llvm-profdata >/dev/null 2>&1; then
            echo "[coverage_merge] ERROR: llvm-profdata not found. Install LLVM/Clang tools." >&2
            return 1
        fi

        local -a profraw_files=()
        while IFS= read -r -d '' file; do
            profraw_files+=("$file")
        done < <(find "$output_dir" -name "*.profraw" -type f -print0 2>/dev/null || true)

        if [[ ${#profraw_files[@]} -eq 0 ]]; then
            echo "[coverage_merge] WARNING: No .profraw files found in $output_dir" >&2
            echo "[coverage_merge] Did you run tests after coverage_collect?" >&2
            return 1
        fi

        echo "[coverage_merge] Found ${#profraw_files[@]} .profraw files"

        local merged_file="$KOG_COVERAGE_ROOT/merged.profdata"
        echo "[coverage_merge] Merging with llvm-profdata..."
        llvm-profdata merge \
            -o "$merged_file" \
            "${profraw_files[@]}" \
            2>&1 || {
                echo "[coverage_merge] ERROR: llvm-profdata merge failed" >&2
                return 1
            }

        echo "[coverage_merge] Merged coverage written to: $merged_file"

    elif [[ "$compiler_id" == "GNU" ]]; then
        # GCC: .gcda files need to be gathered - they are emitted per translation unit
        local -a gcda_files=()
        while IFS= read -r -d '' file; do
            gcda_files+=("$file")
        done < <(find "$output_dir" -name "*.gcda" -type f -print0 2>/dev/null || true)

        if [[ ${#gcda_files[@]} -eq 0 ]]; then
            echo "[coverage_merge] WARNING: No .gcda files found in $output_dir" >&2
            echo "[coverage_merge] Did you run tests after coverage_collect?" >&2
            return 1
        fi

        echo "[coverage_merge] Found ${#gcda_files[@]} .gcda files"
        echo "[coverage_merge] GCC coverage data is ready in: $output_dir"

    elif [[ "$compiler_id" == "MSVC" ]]; then
        # MSVC: coverage data is in .pgc files, merged by the compiler
        local -a pgc_files=()
        while IFS= read -r -d '' file; do
            pgc_files+=("$file")
        done < <(find "$output_dir" -name "*.pgc" -type f -print0 2>/dev/null || true)

        echo "[coverage_merge] Found ${#pgc_files[@]} .pgc files"
        echo "[coverage_merge] MSVC coverage data is in: $output_dir"
    fi

    echo "[coverage_merge] Done. Run: coverage_report"
}

# =============================================================================
# coverage_report - Generate coverage report (HTML or text)
# =============================================================================
# Usage: coverage_report [format] [source-dir]
# Example: coverage_report html
#          coverage_report text
# =============================================================================
coverage_report() {
    local format="${1:-html}"
    local source_dir="${2:-${KOG_CPP_ROOT:-$(pwd)}}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_coverage_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[coverage_report] Generating coverage report..."
    echo "[coverage_report] Platform: $platform, Compiler: $compiler_id"
    echo "[coverage_report] Format: $format"
    echo "[coverage_report] Source dir: $source_dir"

    coverage_ensure_dirs

    local report_file=""

    if [[ "$compiler_id" == "Clang" ]]; then
        local merged_file="$KOG_COVERAGE_ROOT/merged.profdata"
        if [[ ! -f "$merged_file" ]]; then
            echo "[coverage_report] ERROR: Merged profdata not found: $merged_file" >&2
            echo "[coverage_report] Run coverage_merge first." >&2
            return 1
        fi

        if ! command -v llvm-cov >/dev/null 2>&1; then
            echo "[coverage_report] ERROR: llvm-cov not found. Install LLVM/Clang tools." >&2
            return 1
        fi

        # Find the binary
        local binary
        binary=$(find "$KOG_COVERAGE_ROOT" -name "*.exe" -o -name "kano-git" -o -name "kano-git.exe" 2>/dev/null | head -1 || true)

        if [[ -z "$binary" || ! -f "$binary" ]]; then
            echo "[coverage_report] ERROR: Binary not found in $KOG_COVERAGE_ROOT" >&2
            return 1
        fi

        if [[ "$format" == "html" ]]; then
            report_file="$KOG_COVERAGE_REPORT_DIR/index.html"
            echo "[coverage_report] Generating HTML report..."
            llvm-cov show \
                "$binary" \
                --format=html \
                --output-dir="$KOG_COVERAGE_REPORT_DIR" \
                --instr-profile="$merged_file" \
                "${source_dir}" 2>&1 || {
                    echo "[coverage_report] ERROR: llvm-cov report generation failed" >&2
                    return 1
                }
            echo "[coverage_report] HTML report written to: $KOG_COVERAGE_REPORT_DIR/index.html"
        else
            report_file="$KOG_COVERAGE_REPORT_DIR/coverage.txt"
            echo "[coverage_report] Generating text report..."
            llvm-cov report \
                "$binary" \
                --instr-profile="$merged_file" \
                "${source_dir}" > "$report_file" 2>&1 || {
                    echo "[coverage_report] ERROR: llvm-cov report generation failed" >&2
                    return 1
                }
            echo "[coverage_report] Text report written to: $report_file"
        fi

    elif [[ "$compiler_id" == "GNU" ]]; then
        # GCC: use gcov tool
        if ! command -v gcov >/dev/null 2>&1; then
            echo "[coverage_report] ERROR: gcov not found. Install GCC." >&2
            return 1
        fi

        if [[ "$format" == "html" ]]; then
            # Need lcov for HTML reports with GCC
            if ! command -v lcov >/dev/null 2>&1; then
                echo "[coverage_report] WARNING: lcov not found. Installing lcov is recommended for HTML reports." >&2
                echo "[coverage_report] Falling back to text report..." >&2
                format="text"
            else
                report_file="$KOG_COVERAGE_REPORT_DIR/coverage.info"
                echo "[coverage_report] Generating HTML report with lcov..."

                # Capture baseline
                lcov --capture --initial --directory "$KOG_COVERAGE_OUTPUT_DIR" \
                    --output-file "$report_file" --no-checksum >/dev/null 2>&1 || true

                # Capture test data
                lcov --capture --directory "$KOG_COVERAGE_OUTPUT_DIR" \
                    --output-file "$KOG_COVERAGE_REPORT_DIR/coverage_test.info" >/dev/null 2>&1 || true

                # Combine
                lcov --add-tracefile "$report_file" \
                    --add-tracefile "$KOG_COVERAGE_REPORT_DIR/coverage_test.info" \
                    --output-file "$KOG_COVERAGE_REPORT_DIR/coverage_final.info" >/dev/null 2>&1 || true

                # Generate HTML
                genhtml "$KOG_COVERAGE_REPORT_DIR/coverage_final.info" \
                    --output-directory "$KOG_COVERAGE_REPORT_DIR" >/dev/null 2>&1 || true

                echo "[coverage_report] HTML report written to: $KOG_COVERAGE_REPORT_DIR/index.html"
            fi
        fi

        if [[ "$format" != "html" || ! command -v lcov >/dev/null 2>&1 ]]; then
            report_file="$KOG_COVERAGE_REPORT_DIR/coverage.txt"
            echo "[coverage_report] Generating text report with gcov..."

            (
                cd "$source_dir" || exit 1
                find . -name "*.gcno" -type f 2>/dev/null | while IFS= read -r gcno_file; do
                    local dir
                    dir=$(dirname "$gcno_file")
                    local base
                    base=$(basename "$gcno_file" .gcno)
                    (cd "$dir" && gcov -n "$base.gcno" >/dev/null 2>&1 || true) 2>/dev/null || true
                done
            ) || true

            echo "[coverage_report] Text report may be incomplete. Install lcov for full HTML reports."
        fi

    elif [[ "$compiler_id" == "MSVC" ]]; then
        echo "[coverage_report] ERROR: MSVC coverage report generation requires Visual Studio tools." >&2
        echo "[coverage_report] Use Visual Studio's built-in coverage tools or open the .coverage file." >&2
        return 1
    fi

    echo "[coverage_report] Coverage report generation complete."
}

# =============================================================================
# coverage_info - Show current coverage environment and status
# =============================================================================
coverage_info() {
    local env_info

    env_info="$(detect_coverage_environment)"

    echo "=== Coverage Environment ==="
    echo "Platform:        ${env_info%%:*}"
    echo "Compiler:        ${env_info#*:}"
    echo "Coverage Root:   $KOG_COVERAGE_ROOT"
    echo "Output Dir:      $KOG_COVERAGE_OUTPUT_DIR"
    echo "Report Dir:      $KOG_COVERAGE_REPORT_DIR"

    if [[ -d "$KOG_COVERAGE_OUTPUT_DIR" ]]; then
        local count=0
        count=$(find "$KOG_COVERAGE_OUTPUT_DIR" \( -name "*.profraw" -o -name "*.gcda" -o -name "*.pgc" \) 2>/dev/null | wc -l || echo "0")
        echo "Coverage Files:  $count"
    else
        echo "Coverage Files:  0 (output dir not created)"
    fi
}

# =============================================================================
# Main entrypoint
# =============================================================================
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
            coverage_report "$@"
            ;;
        info)
            coverage_info
            ;;
        help|--help|-h)
            echo "Usage: $0 <command> [options]"
            echo ""
            echo "Commands:"
            echo "  collect [preset]   Build with coverage instrumentation (default preset auto-detected)"
            echo "  merge [dir]        Merge collected coverage data (default: coverage output dir)"
            echo "  report [fmt] [src] Generate coverage report (html or text)"
            echo "  info               Show coverage environment and status"
            echo ""
            echo "Environment Variables:"
            echo "  KOG_COVERAGE_ROOT      Base directory for coverage data (default: build/coverage)"
            echo "  KOG_BUILD_ROOT         Build root directory"
            echo "  KOG_CPP_ROOT           C++ source root (CMakeLists.txt location)"
            echo ""
            echo "Workflow:"
            echo "  1. coverage_collect <preset>   # Build with coverage"
            echo "  2. [run tests]                # Execute test suite"
            echo "  3. coverage_merge             # Merge coverage data"
            echo "  4. coverage_report html       # Generate HTML report"
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

# Run if sourced directly (not imported as library)
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    _coverage_main "$@"
fi
