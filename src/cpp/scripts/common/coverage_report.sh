#!/usr/bin/env bash
# =============================================================================
# Coverage Report Workflow Script
# =============================================================================
# Provides functions for:
#   coverage_build   - Build with coverage instrumentation
#   coverage_merge  - Merge .profraw files into merged.profdata
#   coverage_report - Generate HTML/text coverage report
#   coverage_all    - Run full workflow: build + test + merge + report
#
# Platform support:
#   macOS:  native with llvm-cov from Xcode
#   Linux:  native (CI) or Docker (local)
#   Windows: native (CI) with MSVC tooling
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ─────────────────────────────────────────────────────────────────────────────
# Coverage directories
# ─────────────────────────────────────────────────────────────────────────────
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_BUILD_ROOT:-$(pwd)/build}/coverage}"
KOG_COVERAGE_PROFRAW_DIR="$KOG_COVERAGE_ROOT/profraw"
KOG_COVERAGE_PROFDATA="$KOG_COVERAGE_ROOT/merged.profdata"
KOG_COVERAGE_HTML_DIR="$KOG_COVERAGE_ROOT/html"

# ─────────────────────────────────────────────────────────────────────────────
# Utility: Detect platform and compiler
# ─────────────────────────────────────────────────────────────────────────────
detect_coverage_env() {
    local platform
    local compiler_id
    local llvm_cov_path

    platform="$(uname -s 2>/dev/null || echo "unknown")"
    compiler_id="unknown"
    llvm_cov_path=""

    # Detect compiler
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

    # Find llvm-cov
    if [[ "$compiler_id" == "Clang" ]]; then
        if [[ "$platform" == "Darwin" ]]; then
            # macOS: Xcode LLVM
            if [[ -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov" ]]; then
                llvm_cov_path="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"
            elif command -v llvm-cov >/dev/null 2>&1; then
                llvm_cov_path="$(dirname "$(command -v llvm-cov)")"
            fi
        else
            # Linux/Unix: check common paths
            for path in /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin /usr/lib/llvm-16/bin /usr/bin; do
                if [[ -x "$path/llvm-cov" ]]; then
                    llvm_cov_path="$path"
                    break
                fi
            done
            if [[ -z "$llvm_cov_path" ]] && command -v llvm-cov >/dev/null 2>&1; then
                llvm_cov_path="$(dirname "$(command -v llvm-cov)")"
            fi
        fi
    fi

    echo "$platform:$compiler_id:$llvm_cov_path"
}

# ─────────────────────────────────────────────────────────────────────────────
# Utility: Detect host OS (different from target platform for cross-compile)
# ─────────────────────────────────────────────────────────────────────────────
detect_host_os() {
    uname -s 2>/dev/null || echo "unknown"
}

_is_darwin() { [[ "$(detect_host_os)" == "Darwin" ]]; }
_is_linux()  { [[ "$(detect_host_os)" == "Linux" ]]; }
_is_windows(){ [[ "$(detect_host_os)" == MINGW* || "$(detect_host_os)" == CYGWIN* || "$(detect_host_os)" == MSYS* ]]; }

# ─────────────────────────────────────────────────────────────────────────────
# Utility: Ensure directories
# ─────────────────────────────────────────────────────────────────────────────
coverage_ensure_dirs() {
    mkdir -p "$KOG_COVERAGE_PROFRAW_DIR"
    mkdir -p "$KOG_COVERAGE_HTML_DIR"
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_build - Build with coverage instrumentation
# ─────────────────────────────────────────────────────────────────────────────
# Usage: coverage_build [preset]
# Example: coverage_build macos-ninja-clang-coverage
#          coverage_build linux-ninja-clang-coverage
#          coverage_build windows-ninja-msvc-coverage
# ─────────────────────────────────────────────────────────────────────────────
coverage_build() {
    local preset="${1:-}"
    local env_info
    local platform
    local compiler_id
    local llvm_cov_path

    env_info="$(detect_coverage_env)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"
    llvm_cov_path="${env_info##*:}"

    echo "[coverage_build] Starting coverage build..."
    echo "[coverage_build] Host platform: $(detect_host_os)"
    echo "[coverage_build] Target platform: $platform, Compiler: $compiler_id"

    coverage_ensure_dirs

    # Auto-select preset if not provided
    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_darwin; then
                    local arch
                    arch="$(uname -m 2>/dev/null || echo "x86_64")"
                    if [[ "$arch" == "arm64" || "$arch" == "aarch64" ]]; then
                        preset="macos-ninja-clang-arm64-coverage"
                    else
                        preset="macos-ninja-clang-coverage"
                    fi
                elif _is_linux; then
                    preset="linux-ninja-clang-coverage"
                fi
                ;;
            GNU)
                if _is_linux; then
                    preset="linux-ninja-gcc-coverage"
                fi
                ;;
            MSVC)
                preset="windows-ninja-msvc-coverage"
                ;;
        esac
        echo "[coverage_build] Auto-selected preset: $preset"
    fi

    if [[ -z "$preset" ]]; then
        echo "[coverage_build] ERROR: Could not auto-detect preset. Please specify explicitly." >&2
        return 1
    fi

    local host_os
    host_os="$(detect_host_os)"

    # Determine target platform from preset
    local target_platform=""
    if [[ "$preset" == macos-* ]]; then
        target_platform="macos"
    elif [[ "$preset" == linux-* ]]; then
        target_platform="linux"
    elif [[ "$preset" == windows-* ]]; then
        target_platform="windows"
    fi

    echo "[coverage_build] Target platform: $target_platform"

    # Cross-platform build logic
    if [[ "$host_os" == "$target_platform" || "$target_platform" == "windows" ]]; then
        # Native build
        echo "[coverage_build] Native build (host=$host_os, target=$target_platform)"
        (
            if [[ -n "${KOG_CPP_ROOT:-}" ]]; then
                cd "$KOG_CPP_ROOT"
            fi
            cmake --preset "$preset"
            cmake --build --preset "${preset}"
        )
    elif [[ "$target_platform" == "macos" ]]; then
        # macOS build from non-macOS host → use macBuilder
        echo "[coverage_build] Remote macOS build via macBuilder"
        source "$SCRIPT_DIR/macos_remote_build.sh"
        kog_remote_build_macos "$preset" "Debug"
    elif [[ "$target_platform" == "linux" ]]; then
        # Linux build from non-Linux host → use Docker
        if command -v docker >/dev/null 2>&1; then
            echo "[coverage_build] Linux build via Docker"
            coverage_build_linux_via_docker "$preset"
        else
            echo "[coverage_build] ERROR: Docker required for Linux builds on non-Linux host" >&2
            echo "[coverage_build] Or use: coverage_build $preset on a Linux machine" >&2
            return 1
        fi
    else
        echo "[coverage_build] ERROR: Cannot build $target_platform on $host_os without remote build configured" >&2
        return 1
    fi

    echo "[coverage_build] Build complete."
    echo "[coverage_build] Now run: coverage_run_tests [preset] [test-binary]"
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_build_linux_via_docker
# Uses docker cp pattern: build inside container, copy results out
# ─────────────────────────────────────────────────────────────────────────────
coverage_build_linux_via_docker() {
    local preset="${1:-linux-ninja-clang-coverage}"
    local cpp_root="${KOG_CPP_ROOT:-$(pwd)/src/cpp}"
    local container_name="kano-git-coverage-$$"
    local docker_image="ubuntu:24.04"

    echo "[coverage_build_linux_docker] Starting Docker container..."

    # Start container in background, mount source read-only
    docker run -d \
        --name "$container_name" \
        -v "$cpp_root:/workspace/src/cpp:ro" \
        -w /workspace/src/cpp \
        "$docker_image" sleep infinity \
        2>&1 || {
        echo "[coverage_build_linux_docker] ERROR: Failed to start container" >&2
        return 1
    }

    # Install tools and build inside container
    docker exec "$container_name" bash -c "
        set -e
        apt-get update
        apt-get install -y cmake ninja-build clang llvm llvm-tools python3 git
        cmake --preset ${preset}
        cmake --build --preset ${preset}
    " 2>&1 || {
        echo "[coverage_build_linux_docker] ERROR: Docker build failed" >&2
        docker rm -f "$container_name" 2>/dev/null
        return 1
    }

    # Copy build output back to host
    docker cp "$container_name:/workspace/src/cpp/out" "$KOG_COVERAGE_ROOT/linux-out" 2>&1
    docker cp "$container_name:/workspace/src/cpp/_deps" "$KOG_COVERAGE_ROOT/linux-deps" 2>&1 || true

    # Cleanup
    docker rm -f "$container_name" 2>/dev/null

    echo "[coverage_build_linux_docker] Done."
    echo "[coverage_build_linux_docker] Build output copied to: $KOG_COVERAGE_ROOT/linux-out"
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_run_tests - Run tests with coverage instrumentation
# ─────────────────────────────────────────────────────────────────────────────
# Usage: coverage_run_tests [preset] [test-binary]
# Example: coverage_run_tests macos-ninja-clang-coverage kano_git_tui_tests
# ─────────────────────────────────────────────────────────────────────────────
coverage_run_tests() {
    local preset="${1:-}"
    local test_binary="${2:-}"
    local env_info
    local platform
    local compiler_id
    local llvm_cov_path

    env_info="$(detect_coverage_env)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"
    llvm_cov_path="${env_info##*:}"

    echo "[coverage_run_tests] Running tests with coverage..."

    # Auto-detect from preset
    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_darwin; then
                    local arch
                    arch="$(uname -m 2>/dev/null || echo "x86_64")"
                    if [[ "$arch" == "arm64" ]]; then
                        preset="macos-ninja-clang-arm64-coverage"
                    else
                        preset="macos-ninja-clang-coverage"
                    fi
                elif _is_linux; then
                    preset="linux-ninja-clang-coverage"
                fi
                ;;
            GNU)
                preset="linux-ninja-gcc-coverage"
                ;;
            MSVC)
                preset="windows-ninja-msvc-coverage"
                ;;
        esac
    fi

    # Auto-detect test binary
    if [[ -z "$test_binary" ]]; then
        case "$platform" in
            Darwin|Linux)
                test_binary="kano_git_tui_tests"
                ;;
            MINGW*|CYGWIN*|MSYS*|*nt*)
                test_binary="kano_git_tui_tests.exe"
                ;;
        esac
    fi

    # Determine binary path from preset
    # Note: Linux Docker builds copy output to $KOG_COVERAGE_ROOT/linux-out/
    local binary_dir=""
    local cpp_root="${KOG_CPP_ROOT:-$(pwd)/src/cpp}"
    local binary_path=""

    case "$preset" in
        macos-*)
            binary_dir="out/bin/${preset}"
            binary_path="$cpp_root/$binary_dir/$test_binary"
            ;;
        linux-*)
            # Check if Docker build was used (output copied to $KOG_COVERAGE_ROOT/linux-out)
            if [[ -d "$KOG_COVERAGE_ROOT/linux-out/bin/${preset}" ]]; then
                binary_path="$KOG_COVERAGE_ROOT/linux-out/bin/${preset}/$test_binary"
            else
                binary_dir="out/bin/${preset}"
                binary_path="$cpp_root/$binary_dir/$test_binary"
            fi
            ;;
        windows-*)
            binary_dir="out/bin/${preset}"
            binary_path="$cpp_root/$binary_dir/$test_binary"
            ;;
    esac

    if [[ ! -f "$binary_path" ]]; then
        echo "[coverage_run_tests] ERROR: Binary not found: $binary_path" >&2
        return 1
    fi

    echo "[coverage_run_tests] Binary: $binary_path"
    echo "[coverage_run_tests] Profile output: $KOG_COVERAGE_PROFRAW_DIR"

    # Clean old profraw files
    rm -f "$KOG_COVERAGE_PROFRAW_DIR"/*.profraw 2>/dev/null || true

    # Set LLVM_PROFILE_FILE and run
    export LLVM_PROFILE_FILE="$KOG_COVERAGE_PROFRAW_DIR/%m.profraw"

    (
        cd "$cpp_root"
        "$binary_path"
    )

    echo "[coverage_run_tests] Tests complete. Profile data in: $KOG_COVERAGE_PROFRAW_DIR"
    echo "[coverage_run_tests] Found: $(find "$KOG_COVERAGE_PROFRAW_DIR" -name "*.profraw" 2>/dev/null | wc -l) .profraw files"
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_merge - Merge .profraw files into merged.profdata
# ─────────────────────────────────────────────────────────────────────────────
# Usage: coverage_merge [output-file]
# Example: coverage_merge
#          coverage_merge custom.profdata
# ─────────────────────────────────────────────────────────────────────────────
coverage_merge() {
    local output_file="${1:-$KOG_COVERAGE_PROFDATA}"
    local env_info
    local platform
    local compiler_id
    local llvm_cov_path

    env_info="$(detect_coverage_env)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"
    llvm_cov_path="${env_info##*:}"

    echo "[coverage_merge] Merging coverage profiles..."
    echo "[coverage_merge] Platform: $platform, Compiler: $compiler_id"
    echo "[coverage_merge] LLVM-cov path: ${llvm_cov_path:-not found}"

    coverage_ensure_dirs

    if [[ "$compiler_id" == "Clang" ]]; then
        if [[ -z "$llvm_cov_path" ]] && ! command -v llvm-profdata >/dev/null 2>&1; then
            echo "[coverage_merge] ERROR: llvm-profdata not found. Install LLVM/Clang tools." >&2
            return 1
        fi

        local llvm_profdata="${llvm_cov_path:+$llvm_cov_path/}llvm-profdata"
        if [[ ! -x "$llvm_profdata" ]] && ! command -v llvm-profdata >/dev/null 2>&1; then
            echo "[coverage_merge] ERROR: llvm-profdata not executable: $llvm_profdata" >&2
            return 1
        fi
        if [[ -x "$llvm_profdata" ]]; then
            llvm_profdata="llvm-profdata"
        fi

        local -a profraw_files=()
        while IFS= read -r -d '' file; do
            profraw_files+=("$file")
        done < <(find "$KOG_COVERAGE_PROFRAW_DIR" -name "*.profraw" -type f -print0 2>/dev/null || true)

        if [[ ${#profraw_files[@]} -eq 0 ]]; then
            echo "[coverage_merge] WARNING: No .profraw files found in $KOG_COVERAGE_PROFRAW_DIR" >&2
            echo "[coverage_merge] Did you run coverage_run_tests first?" >&2
            return 1
        fi

        echo "[coverage_merge] Found ${#profraw_files[@]} .profraw files"

        mkdir -p "$(dirname "$output_file")"
        "$llvm_profdata" merge -o "$output_file" "${profraw_files[@]}" 2>&1 || {
            echo "[coverage_merge] ERROR: llvm-profdata merge failed" >&2
            return 1
        }

        echo "[coverage_merge] Merged profile written to: $output_file"

    elif [[ "$compiler_id" == "GNU" ]]; then
        echo "[coverage_merge] GCC coverage: .gcda files are in build directory"
        echo "[coverage_merge] Use gcov tool to generate coverage reports"

    elif [[ "$compiler_id" == "MSVC" ]]; then
        echo "[coverage_merge] MSVC coverage: /PROFILE data in build directory"
        echo "[coverage_merge] Use Microsoft.CodeCoverage.Console or Visual Studio for reports"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_report - Generate HTML/text coverage report
# ─────────────────────────────────────────────────────────────────────────────
# Usage: coverage_report [preset] [test-binary]
# Example: coverage_report
#          coverage_report macos-ninja-clang-coverage kano_git_tui_tests
# ─────────────────────────────────────────────────────────────────────────────
coverage_report() {
    local preset="${1:-}"
    local test_binary="${2:-}"
    local env_info
    local platform
    local compiler_id
    local llvm_cov_path

    env_info="$(detect_coverage_env)"
    platform="${env_info%%:*}"
    compiler_id="${env_info#*:}"
    llvm_cov_path="${env_info##*:}"

    echo "[coverage_report] Generating coverage report..."
    echo "[coverage_report] Platform: $platform, Compiler: $compiler_id"

    coverage_ensure_dirs

    # Auto-detect preset
    if [[ -z "$preset" ]]; then
        case "$compiler_id" in
            Clang)
                if _is_darwin; then
                    local arch
                    arch="$(uname -m 2>/dev/null || echo "x86_64")"
                    if [[ "$arch" == "arm64" ]]; then
                        preset="macos-ninja-clang-arm64-coverage"
                    else
                        preset="macos-ninja-clang-coverage"
                    fi
                elif _is_linux; then
                    preset="linux-ninja-clang-coverage"
                fi
                ;;
            GNU)
                preset="linux-ninja-gcc-coverage"
                ;;
            MSVC)
                preset="windows-ninja-msvc-coverage"
                ;;
        esac
    fi

    # Auto-detect test binary
    if [[ -z "$test_binary" ]]; then
        case "$platform" in
            Darwin|Linux)
                test_binary="kano_git_tui_tests"
                ;;
            MINGW*|CYGWIN*|MSYS*|*nt*)
                test_binary="kano_git_tui_tests.exe"
                ;;
        esac
    fi

    local cpp_root="${KOG_CPP_ROOT:-$(pwd)/src/cpp}"
    local binary_dir="out/bin/${preset}"
    local binary_path="$cpp_root/$binary_dir/$test_binary"

    if [[ ! -f "$binary_path" ]]; then
        echo "[coverage_report] ERROR: Binary not found: $binary_path" >&2
        return 1
    fi

    if [[ ! -f "$KOG_COVERAGE_PROFDATA" ]]; then
        echo "[coverage_report] ERROR: Merged profile not found: $KOG_COVERAGE_PROFDATA" >&2
        echo "[coverage_report] Run coverage_merge first." >&2
        return 1
    fi

    if [[ "$compiler_id" == "Clang" ]]; then
        if [[ -z "$llvm_cov_path" ]] && ! command -v llvm-cov >/dev/null 2>&1; then
            echo "[coverage_report] ERROR: llvm-cov not found." >&2
            return 1
        fi

        local llvm_cov="${llvm_cov_path:+$llvm_cov_path/}llvm-cov"
        if [[ ! -x "$llvm_cov" ]] && ! command -v llvm-cov >/dev/null 2>&1; then
            echo "[coverage_report] ERROR: llvm-cov not executable: $llvm_cov" >&2
            return 1
        fi
        if [[ -x "$llvm_cov" ]]; then
            llvm_cov="llvm-cov"
        fi

        # HTML report
        mkdir -p "$KOG_COVERAGE_HTML_DIR"
        echo "[coverage_report] Generating HTML report..."
        "$llvm_cov" show \
            "$binary_path" \
            -instr-profile="$KOG_COVERAGE_PROFDATA" \
            --format=html \
            --output-dir="$KOG_COVERAGE_HTML_DIR" \
            --ignore-filename-regex="_deps|catch2|ftxui|thirdparty|build|\.vcpkg" 2>&1 || {
            echo "[coverage_report] WARNING: Some files not found (normal for deps)"
        }

        # Text summary
        echo ""
        echo "[coverage_report] Text summary:"
        "$llvm_cov" report \
            "$binary_path" \
            -instr-profile="$KOG_COVERAGE_PROFDATA" \
            --ignore-filename-regex="_deps|catch2|ftxui|thirdparty|build|\.vcpkg" 2>&1 || true

        echo ""
        echo "[coverage_report] HTML report: $KOG_COVERAGE_HTML_DIR/index.html"

    elif [[ "$compiler_id" == "GNU" ]]; then
        echo "[coverage_report] Use gcov to generate coverage reports from .gcda files"
        echo "[coverage_report] gcda files in: $KOG_COVERAGE_PROFRAW_DIR"

    elif [[ "$compiler_id" == "MSVC" ]]; then
        echo "[coverage_report] MSVC coverage data in build directory"
        echo "[coverage_report] Use Microsoft.CodeCoverage.Console: coverage analyze /in:input.coverage /out:output.coverage"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_all - Run full coverage workflow
# ─────────────────────────────────────────────────────────────────────────────
# Usage: coverage_all [preset]
# Example: coverage_all macos-ninja-clang-coverage
#          coverage_all linux-ninja-clang-coverage
# ─────────────────────────────────────────────────────────────────────────────
coverage_all() {
    local preset="${1:-}"

    echo "========================================"
    echo "  Coverage Workflow"
    echo "========================================"

    coverage_build "$preset"
    coverage_run_tests "$preset"
    coverage_merge
    coverage_report "$preset"

    echo ""
    echo "========================================"
    echo "  Coverage Complete"
    echo "========================================"
    echo "Reports:"
    echo "  HTML: $KOG_COVERAGE_HTML_DIR/index.html"
    echo "  Summary: Run 'coverage_report' for text output"
}

# ─────────────────────────────────────────────────────────────────────────────
# coverage_info - Show current coverage environment
# ─────────────────────────────────────────────────────────────────────────────
coverage_info() {
    local env_info
    env_info="$(detect_coverage_env)"

    echo "=== Coverage Environment ==="
    echo "Host OS:        $(detect_host_os)"
    echo "Target Platform: ${env_info%%:*}"
    echo "Compiler:       ${env_info#*:}"
    echo "llvm-cov path: ${env_info##*:}"
    echo ""
    echo "Directories:"
    echo "  Coverage Root: $KOG_COVERAGE_ROOT"
    echo "  Profraw Dir:  $KOG_COVERAGE_PROFRAW_DIR"
    echo "  Merged Data:  $KOG_COVERAGE_PROFDATA"
    echo "  HTML Output:  $KOG_COVERAGE_HTML_DIR"
}

# ─────────────────────────────────────────────────────────────────────────────
# Main entrypoint
# ─────────────────────────────────────────────────────────────────────────────
_coverage_main() {
    local command="${1:-}"

    case "$command" in
        build)
            shift
            coverage_build "$@"
            ;;
        test|run-tests)
            shift
            coverage_run_tests "$@"
            ;;
        merge)
            shift
            coverage_merge "$@"
            ;;
        report)
            shift
            coverage_report "$@"
            ;;
        all)
            shift
            coverage_all "$@"
            ;;
        info)
            coverage_info
            ;;
        help|--help|-h)
            echo "Usage: $0 <command> [options]"
            echo ""
            echo "Commands:"
            echo "  build [preset]       Build with coverage instrumentation"
            echo "  test [preset] [bin]  Run tests to generate .profraw files"
            echo "  merge [file]        Merge .profraw into merged.profdata"
            echo "  report [preset] [bin] Generate HTML/text coverage report"
            echo "  all [preset]         Run full workflow: build + test + merge + report"
            echo "  info                 Show coverage environment"
            echo ""
            echo "Examples:"
            echo "  $0 all macos-ninja-clang-coverage"
            echo "  $0 all linux-ninja-clang-coverage"
            echo "  $0 build linux-ninja-clang-coverage"
            echo "  $0 test"
            echo "  $0 merge"
            echo "  $0 report"
            echo ""
            echo "Environment Variables:"
            echo "  KOG_COVERAGE_ROOT    Base directory (default: build/coverage)"
            echo "  KOG_BUILD_ROOT      Build root directory"
            echo "  KOG_CPP_ROOT        C++ source root"
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

# Run if called directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    _coverage_main "$@"
fi
