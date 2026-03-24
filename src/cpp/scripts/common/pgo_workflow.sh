#!/usr/bin/env bash
# =============================================================================
# PGO (Profile-Guided Optimization) Workflow Script
# =============================================================================
# Provides functions for:
#   pgo_collect  - Build with PGO instrumentation and run tests
#   pgo_merge    - Merge collected profile data into single .profdata/.gcda
#   pgo_use     - Build optimized binary using collected profile data
# =============================================================================
set -euo pipefail

# Detect platform and compiler
detect_pgo_environment() {
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

# Default PGO directories
KOG_PGO_ROOT="${KOG_PGO_ROOT:-${KOG_BUILD_ROOT:-$(pwd)/build}/pgo}"
KOG_PGO_COLLECT_DIR="$KOG_PGO_ROOT/collect"
KOG_PGO_PROFILE_DIR="$KOG_PGO_ROOT/profiles"
KOG_PGO_MERGED_FILE="$KOG_PGO_PROFILE_DIR/merged.profdata"

# Unix detect
_is_unix() {
    [[ "$(uname -s 2>/dev/null || echo "unknown")" != "CYGWIN"* && "$(uname -s 2>/dev/null || echo "unknown")" != MINGW* && "$(uname -s 2>/dev/null || echo "unknown")" != MSYS* ]]
}

# Ensure directories exist
pgo_ensure_dirs() {
    mkdir -p "$KOG_PGO_COLLECT_DIR"
    mkdir -p "$KOG_PGO_PROFILE_DIR"
}

# =============================================================================
# pgo_collect - Build with PGO instrumentation and run tests to collect profile
# =============================================================================
# Usage: pgo_collect [preset-name]
# Example: pgo_collect linux-ninja-gcc-pgo-collect
# =============================================================================
pgo_collect() {
    local preset="${1:-}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_pgo_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[pgo_collect] Starting PGO collection..."
    echo "[pgo_collect] Platform: $platform, Compiler: $compiler_id"

    pgo_ensure_dirs

    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_unix; then
                    preset="linux-ninja-clang-pgo-collect"
                else
                    preset="windows-ninja-clang-pgo-collect"
                fi
                ;;
            GNU)
                if _is_unix; then
                    preset="linux-ninja-gcc-pgo-collect"
                else
                    echo "[pgo_collect] ERROR: GCC PGO collect on Windows not supported" >&2
                    return 1
                fi
                ;;
            MSVC)
                preset="windows-ninja-msvc-pgo-collect"
                ;;
            *)
                echo "[pgo_collect] ERROR: Unknown compiler: $compiler_id" >&2
                return 1
                ;;
        esac
        echo "[pgo_collect] Auto-selected preset: $preset"
    fi

    echo "[pgo_collect] Cleaning PGO collect directory..."
    if [[ "$platform" == "Darwin" ]]; then
        # macOS uses .profraw files
        rm -f "$KOG_PGO_COLLECT_DIR"/*.profraw 2>/dev/null || true
    elif [[ "$compiler_id" == "GNU" ]]; then
        # GCC uses .gcda files
        rm -f "$KOG_PGO_COLLECT_DIR"/*.gcda 2>/dev/null || true
    fi

    echo "[pgo_collect] Configuring with preset: $preset"
    (
        if [[ -n "${KOG_CPP_ROOT:-}" ]]; then
            cd "$KOG_CPP_ROOT"
        fi

        cmake --preset "$preset"
        cmake --build --preset "${preset}" --config Debug
    )

    echo "[pgo_collect] Build complete. Profile data written to: $KOG_PGO_COLLECT_DIR"
    echo "[pgo_collect] Run your tests now to generate profile data."
    echo "[pgo_collect] Then run: pgo_merge"
}

# =============================================================================
# pgo_merge - Merge collected profile data into single usable profile file
# =============================================================================
# Usage: pgo_merge [output-file]
# Example: pgo_merge                     # uses default: merged.profdata
#          pgo_merge custom.profdata
# =============================================================================
pgo_merge() {
    local output_file="${1:-$KOG_PGO_MERGED_FILE}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_pgo_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[pgo_merge] Starting profile data merge..."
    echo "[pgo_merge] Platform: $platform, Compiler: $compiler_id"

    pgo_ensure_dirs

    if [[ "$compiler_id" == "Clang" ]]; then
        # Clang: use llvm-profdata merge
        if ! command -v llvm-profdata >/dev/null 2>&1; then
            echo "[pgo_merge] ERROR: llvm-profdata not found. Install LLVM/Clang tools." >&2
            return 1
        fi

        local -a profraw_files=()
        while IFS= read -r -d '' file; do
            profraw_files+=("$file")
        done < <(find "$KOG_PGO_COLLECT_DIR" -name "*.profraw" -type f -print0 2>/dev/null || true)

        if [[ ${#profraw_files[@]} -eq 0 ]]; then
            echo "[pgo_merge] WARNING: No .profraw files found in $KOG_PGO_COLLECT_DIR" >&2
            echo "[pgo_merge] Did you run tests after pgo_collect?" >&2
            return 1
        fi

        echo "[pgo_merge] Found ${#profraw_files[@]} .profraw files"
        echo "[pgo_merge] Merging with llvm-profdata..."

        llvm-profdata merge \
            -o "$output_file" \
            "${profraw_files[@]}" \
            2>&1 || {
                echo "[pgo_merge] ERROR: llvm-profdata merge failed" >&2
                return 1
            }

        echo "[pgo_merge] Merged profile written to: $output_file"

    elif [[ "$compiler_id" == "GNU" ]]; then
        # GCC: .gcda files are automatically merged by the linker
        # But we can copy them to the profile directory for backup
        local -a gcda_files=()
        while IFS= read -r -d '' file; do
            gcda_files+=("$file")
        done < <(find "$KOG_PGO_COLLECT_DIR" -name "*.gcda" -type f -print0 2>/dev/null || true)

        if [[ ${#gcda_files[@]} -eq 0 ]]; then
            echo "[pgo_merge] WARNING: No .gcda files found in $KOG_PGO_COLLECT_DIR" >&2
            echo "[pgo_merge] Did you run tests after pgo_collect?" >&2
            return 1
        fi

        echo "[pgo_merge] Found ${#gcda_files[@]} .gcda files"
        echo "[pgo_merge] GCC profile data is auto-merged. Copying to: $output_file"

        mkdir -p "$(dirname "$output_file")"
        cp -t "$(dirname "$output_file")" "${gcda_files[@]}" || true
        echo "[pgo_merge] GCC profile data copied to: $output_file"

    elif [[ "$compiler_id" == "MSVC" ]]; then
        # MSVC: .pgd files are generated in the build directory
        local -a pgd_files=()
        while IFS= read -r -d '' file; do
            pgd_files+=("$file")
        done < <(find "$KOG_PGO_COLLECT_DIR" -name "*.pgd" -type f -print0 2>/dev/null || true)

        if [[ ${#pgd_files[@]} -eq 0 ]]; then
            echo "[pgo_merge] WARNING: No .pgd files found in $KOG_PGO_COLLECT_DIR" >&2
            echo "[pgo_merge] Did you run tests after pgo_collect?" >&2
            return 1
        fi

        echo "[pgo_merge] Found ${#pgd_files[@]} .pgd files"
        echo "[pgo_merge] MSVC profile data at: $KOG_PGO_COLLECT_DIR"
        echo "[pgo_merge] Copy .pgd files to your PGO use build directory manually."
    fi

    echo "[pgo_merge] Done."
}

# =============================================================================
# pgo_use - Build optimized binary using collected profile data
# =============================================================================
# Usage: pgo_use [preset-name] [profdata-file]
# Example: pgo_use linux-ninja-gcc-pgo-use merged.profdata
# =============================================================================
pgo_use() {
    local preset="${1:-}"
    local profdata_file="${2:-}"
    local env_info
    local platform
    local compiler_id

    env_info="$(detect_pgo_environment)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"

    echo "[pgo_use] Starting PGO-optimized build..."
    echo "[pgo_use] Platform: $platform, Compiler: $compiler_id"

    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_unix; then
                    preset="linux-ninja-clang-pgo-use"
                else
                    preset="windows-ninja-clang-pgo-use"
                fi
                ;;
            GNU)
                if _is_unix; then
                    preset="linux-ninja-gcc-pgo-use"
                else
                    echo "[pgo_use] ERROR: GCC PGO use on Windows not supported" >&2
                    return 1
                fi
                ;;
            MSVC)
                preset="windows-ninja-msvc-pgo-use"
                ;;
            *)
                echo "[pgo_use] ERROR: Unknown compiler: $compiler_id" >&2
                return 1
                ;;
        esac
        echo "[pgo_use] Auto-selected preset: $preset"
    fi

    if [[ "$compiler_id" == "Clang" && -z "$profdata_file" ]]; then
        profdata_file="$KOG_PGO_MERGED_FILE"
    fi

    echo "[pgo_use] Configuring with preset: $preset"
    echo "[pgo_use] Profile data: ${profdata_file:-N/A (GCC/MSVC auto-detect)}"

    (
        if [[ -n "${KOG_CPP_ROOT:-}" ]]; then
            cd "$KOG_CPP_ROOT"
        fi

        # For Clang, pass the profdata file path
        if [[ "$compiler_id" == "Clang" && -n "$profdata_file" && -f "$profdata_file" ]]; then
            export KOG_PGO_PROFDATA="$profdata_file"
            cmake --preset "$preset" \
                -DCMAKE_PROF_DATA="$profdata_file"
        else
            cmake --preset "$preset"
        fi

        cmake --build --preset "${preset}" --config Release
    )

    echo "[pgo_use] PGO-optimized build complete."
    echo "[pgo_use] Output directory: $KOG_BUILD_ROOT/release (or equivalent)"
}

# =============================================================================
# pgo_info - Show current PGO environment and status
# =============================================================================
pgo_info() {
    local env_info

    env_info="$(detect_pgo_environment)"

    echo "=== PGO Environment ==="
    echo "Platform:       ${env_info%%:*}"
    echo "Compiler:       ${env_info#*:}"
    echo "PGO Root:       $KOG_PGO_ROOT"
    echo "Collect Dir:    $KOG_PGO_COLLECT_DIR"
    echo "Profile Dir:    $KOG_PGO_PROFILE_DIR"
    echo "Merged File:    $KOG_PGO_MERGED_FILE"

    if [[ -d "$KOG_PGO_COLLECT_DIR" ]]; then
        local count
        count=$(find "$KOG_PGO_COLLECT_DIR" \( -name "*.profraw" -o -name "*.gcda" -o -name "*.pgd" \) 2>/dev/null | wc -l || echo "0")
        echo "Profile Files:  $count"
    else
        echo "Profile Files:  0 (collect dir not created)"
    fi
}

# =============================================================================
# Main entrypoint
# =============================================================================
_pgo_main() {
    local command="${1:-}"

    case "$command" in
        collect)
            shift
            pgo_collect "$@"
            ;;
        merge)
            shift
            pgo_merge "$@"
            ;;
        use)
            shift
            pgo_use "$@"
            ;;
        info)
            pgo_info
            ;;
        help|--help|-h)
            echo "Usage: $0 <command> [options]"
            echo ""
            echo "Commands:"
            echo "  collect [preset]   Build with PGO instrumentation (default preset auto-detected)"
            echo "  merge [file]       Merge collected profile data (default: merged.profdata)"
            echo "  use [preset] [prof] Build with PGO using profile data"
            echo "  info               Show PGO environment and status"
            echo ""
            echo "Environment Variables:"
            echo "  KOG_PGO_ROOT       Base directory for PGO data (default: build/pgo)"
            echo "  KOG_BUILD_ROOT     Build root directory"
            echo "  KOG_CPP_ROOT       C++ source root (CMakeLists.txt location)"
            echo ""
            echo "Workflow:"
            echo "  1. pgo_collect <preset>   # Build with instrumentation"
            echo "  2. [run tests]            # Execute test suite"
            echo "  3. pgo_merge              # Merge .profraw/.gcda files"
            echo "  4. pgo_use <preset> <data># Build optimized binary"
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
    _pgo_main "$@"
fi
