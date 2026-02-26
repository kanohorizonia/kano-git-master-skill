# kano-git CLI

C++20 command-line interface for [Kano Git Master](../SKILL.md).

## Overview

The CLI wraps existing shell scripts into a unified binary (`kano-git` / `kog`) with:
- Structured command tree (subcommands, flags, help)
- Cross-platform support (Windows, macOS, Linux)
- AI provider selection for smart commands
- Future: parallel execution for multi-repo operations

## Quick Start

### Prerequisites

- **CMake** ≥ 3.21
- **C++20 compiler** (GCC 13+, Clang 16+, MSVC 19.38+)
- **vcpkg** (set `VCPKG_ROOT` environment variable)

### Build

```bash
# Windows host (Git Bash)
./src/cpp/build/script/windows/build_windows_ninja_msvc_debug.sh
./src/cpp/build/script/windows/build_windows_ninja_msvc_release.sh

# Windows host -> Linux via Docker
./src/cpp/build/script/windows/build_linux_ninja_gcc_debug_via_docker.sh
./src/cpp/build/script/windows/build_linux_ninja_gcc_release_via_docker.sh

# Linux host
./src/cpp/build/script/linux/build_linux_ninja_gcc_debug.sh
./src/cpp/build/script/linux/build_linux_ninja_gcc_release.sh

# macOS host (Intel)
./src/cpp/build/script/macos/build_macos_ninja_clang_x64_debug.sh
./src/cpp/build/script/macos/build_macos_ninja_clang_x64_release.sh

# macOS host (Apple Silicon)
./src/cpp/build/script/macos/build_macos_ninja_clang_arm64_debug.sh
./src/cpp/build/script/macos/build_macos_ninja_clang_arm64_release.sh
```

### Run

```bash
# Windows (MSVC Ninja preset)
./src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe version

# Linux (GCC Ninja preset)
./src/cpp/build/bin/linux-ninja-gcc/release/kano-git version

# Generic artifact layout
./src/cpp/build/bin/<preset>/<config>/kano-git[.exe]
```

### Python Launcher (alternative)

```bash
# Add scripts/ to PATH, then:
kano-git commit
kog commit    # short alias
```

### Launcher mode (developer vs installed package)

- Default behavior (no marker): launcher assumes **developer mode** and auto-runs prerequisite + build scripts when C++ binary is missing.
- Installed-package behavior: if marker file exists, launcher assumes **packaged install mode** and will **not** auto-build.
- Marker path:
  - default: `.kano-installed-marker` at repo root
  - override: `KANO_GIT_INSTALL_MARKER=/absolute/path/to/marker`
- In packaged-install mode, if binary is missing, launcher prints an installation-corruption warning and asks user to reinstall.

## Command Tree

| Command | Description |
|---------|-------------|
| `commit` | AI-powered commit message generation |
| `resolve` | AI-powered conflict resolution |
| `sync` | Repository synchronization (origin-latest, upstream, stable-dev) |
| `push` | Smart multi-remote push |
| `worktree` | Git worktree management (create, list, remove, sync) |
| `subtree` | Git subtree operations (add, pull, push, split, list) |
| `submodule` | Enhanced submodule management |
| `scalar` | Git Scalar mono-repo performance |
| `p4` | Git-Perforce bridge |
| `svn` | Git-Subversion bridge |
| `branch` | Branch operations (rebase-upstream, compare, cherry-pick) |
| `workspace` | Multi-repo workspace operations |
| `clone` | Smart clone with upstream support |
| `doctor` | Environment and repo health checks |
| `version` | Show version |

## Architecture

```
src/
├── cpp/
│   ├── code/systems/kano_git_core/ # static library (C++20)
│   │   ├── shell_executor   # Process spawning
│   │   ├── command_registry # Command routing
│   │   └── commands/        # Command implementations
│   └── code/apps/kano_git_cli/   # Thin CLI frontend
│       └── main.cpp         # CLI11 entry point
└── shell/                   # Shell scripts execution backend
```

The CLI is a thin orchestrator — all actual Git logic lives in the existing shell scripts under `src/shell/`.
The `kano_git_core` library is designed for reuse by future TUI/GUI frontends.

## Project Structure

```
src/
├── cpp/
│   ├── CMakeLists.txt          # Build configuration (C++20 modules)
│   ├── CMakePresets.json       # Multi-config presets (Debug/Release)
│   ├── vcpkg.json              # Dependencies (CLI11)
│   ├── build/script/           # Host/preset build wrappers
│   ├── code/systems/kano_git_core/  # Core static library
│   ├── code/apps/kano_git_cli/      # CLI executable
│   ├── code/thirdparty/cli11/       # Vendored CLI11 source
│   ├── build/bin/              # Final executables by preset/config
│   ├── build/lib/              # Libraries by preset/config
│   └── build/_intermediate/    # CMake/Ninja/MSBuild intermediates
└── shell/                      # Shell execution backend
    ├── core/
    ├── workspace/
    └── ...

scripts/                        # Python launchers
├── kano-git / kano-git.bat
└── kog / kog.bat
```
