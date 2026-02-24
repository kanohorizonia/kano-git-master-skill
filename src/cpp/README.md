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
- **C++23 compiler** (GCC 13+, Clang 16+, MSVC 19.38+)
- **vcpkg** (set `VCPKG_ROOT` environment variable)

### Build

```bash
# Unix
cd src/cpp
./build.sh                  # Release build
./build.sh --debug          # Debug build
./build.sh --clean          # Clean rebuild

# Windows
cd src/cpp
.\build.ps1                 # Release build
.\build.ps1 -Config debug   # Debug build
.\build.ps1 -Clean          # Clean rebuild
```

### Run

```bash
./src/cpp/build/release/kano-git version      # Show version
./src/cpp/build/release/kano-git help          # Show all commands
./src/cpp/build/release/kano-git commit        # AI-powered commit
./src/cpp/build/release/kano-git push          # Smart multi-remote push
```

### Python Launcher (alternative)

```bash
# Add scripts/ to PATH, then:
kano-git commit
kog commit    # short alias
```

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
│   ├── kano-git-core/       # static library (C++23)
│   │   ├── shell_executor   # Process spawning
│   │   ├── command_registry # Command routing
│   │   └── commands/        # Command implementations
│   └── kog-cli/             # Thin CLI frontend
│       └── main.cpp         # CLI11 entry point
└── shell/                   # Shell scripts execution backend
```

The CLI is a thin orchestrator — all actual Git logic lives in the existing shell scripts under `src/shell/`.
The `kano-git-core` library is designed for reuse by future TUI/GUI frontends.

## Project Structure

```
src/
├── cpp/
│   ├── CMakeLists.txt          # Build configuration (C++23)
│   ├── CMakePresets.json       # Debug/Release presets
│   ├── vcpkg.json              # Dependencies (CLI11, fmt)
│   ├── build.sh / build.ps1    # Build scripts
│   ├── kano-git-core/          # Core static library
│   │   ├── CMakeLists.txt
│   │   ├── include/            # Public headers
│   │   └── src/                # Implementations & commands/
│   └── kog-cli/                # CLI executable
│       ├── CMakeLists.txt
│       └── main.cpp
└── shell/                      # Shell execution backend
    ├── core/
    ├── workspace/
    └── ...

scripts/                        # Python launchers
├── kano-git / kano-git.bat
└── kog / kog.bat
```
