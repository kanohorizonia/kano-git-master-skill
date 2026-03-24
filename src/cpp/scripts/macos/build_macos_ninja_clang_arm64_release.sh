#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
export KOG_BUILD_USE_LLVM=1

source "$SCRIPT_DIR/../common/unix_preset_build.sh"

resolve_macos_compiler_flags() {
	local -n out_ref="$1"
	local llvm_prefix=""
	local sdk_path=""

	out_ref=(
		-DCMAKE_C_COMPILER=clang
		-DCMAKE_CXX_COMPILER=clang++
		-DKOG_PRESET_NAME=macos-make-clang-arm64
	)

	if [[ "${KOG_BUILD_USE_LLVM:-0}" == "1" ]]; then
		llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
		if [[ -z "$llvm_prefix" || ! -x "$llvm_prefix/bin/clang" || ! -x "$llvm_prefix/bin/clang++" ]]; then
			echo "Homebrew LLVM is required for --llvm mode. Install with: brew install llvm" >&2
			return 1
		fi

		sdk_path="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"

		out_ref=(
			"-DCMAKE_C_COMPILER=$llvm_prefix/bin/clang"
			"-DCMAKE_CXX_COMPILER=$llvm_prefix/bin/clang++"
			-DKOG_PRESET_NAME=macos-make-llvm-arm64
		)
		if [[ -n "$sdk_path" ]]; then
			out_ref+=("-DCMAKE_OSX_SYSROOT=$sdk_path")
		fi
	fi
}

if command -v ninja >/dev/null 2>&1; then
	kog_run_unix_preset "macos-ninja-clang-arm64" "macos-ninja-clang-arm64-release"
	exit 0
fi

echo "ninja not found. Falling back to Unix Makefiles for macOS arm64 release build." >&2
compiler_flags=()
resolve_macos_compiler_flags compiler_flags
modules_value="OFF"
if [[ "${KOG_BUILD_ENABLE_MODULES:-0}" == "1" ]]; then
	modules_value="ON"
fi

(
	cd "$KOG_CPP_ROOT"
	kog_collect_build_metadata
	cmake -S . -B out/obj/macos-make-clang-arm64 \
		-G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_OSX_ARCHITECTURES=arm64 \
		"${compiler_flags[@]}" \
		-DKOG_ENABLE_MODULES="$modules_value"
	cmake --build out/obj/macos-make-clang-arm64 --parallel
)
