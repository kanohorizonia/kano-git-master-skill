# kano-git CLI

C++20 command-line interface for [Kano Git Master](../SKILL.md).

## Overview

The CLI provides a unified binary (`kano-git` / `kog`) with:
- Structured command tree (subcommands, flags, help)
- Cross-platform support (Windows, macOS, Linux)
- AI provider selection for AI-assisted commands
- Future: parallel execution for multi-repo operations

## Quick Start

### Prerequisites

- **pixi** (recommended for repo-local tools; see `../../docs/development/pixi.md`)
- **CMake** ≥ 3.21
- **C++20 compiler** (GCC 13+, Clang 16+, MSVC 19.38+)
- **vcpkg** (set `VCPKG_ROOT` environment variable)

`pixi` does not replace `vcpkg` here.

- `pixi` manages repo-local tools such as `bash`, `cmake`, `ninja`, `git`, and `python`
- `vcpkg` still provides native C++ libraries from `src/cpp/vcpkg.json`
- platform prerequisite scripts still handle host-specific system packages such as Visual Studio Build Tools
- the prerequisite bootstrap scripts now skip pixi-provided common tools where possible, while keeping system compiler/toolchain setup in the platform scripts

### Build

Recommended repo-root flow:

```bash
pixi install
pixi run env-summary
pixi run build
```

Direct script flow remains the source of truth and is what the pixi tasks call:

```bash
# Windows host (Git Bash)
bash src/cpp/scripts/windows/ninja-msvc-debug.sh
bash src/cpp/scripts/windows/ninja-msvc-release.sh
bash src/cpp/scripts/windows/msbuild-generate-sln.sh
bash src/cpp/scripts/windows/msbuild-generate-sln.sh --open

# Linux host
bash src/cpp/scripts/linux/ninja-gcc-debug.sh
bash src/cpp/scripts/linux/ninja-gcc-release.sh

# macOS host (Intel)
bash src/cpp/scripts/macos/ninja-clang-x64-debug.sh
bash src/cpp/scripts/macos/ninja-clang-x64-release.sh

# macOS host (Apple Silicon)
bash src/cpp/scripts/macos/ninja-clang-arm64-debug.sh
bash src/cpp/scripts/macos/ninja-clang-arm64-release.sh
```

### Run

```bash
# Windows (MSVC Ninja preset)
./src/cpp/out/bin/windows-ninja-msvc/release/kano-git.exe version

# Linux (GCC Ninja preset)
./src/cpp/out/bin/linux-ninja-gcc/release/kano-git version

# Generic artifact layout
./src/cpp/out/bin/<preset>/<config>/kano-git[.exe]
```

### Visual Studio solution workflow (Windows)

If you want a `.sln` for Visual Studio debugging or to open `windows.coverage` in the native Microsoft tooling, generate the Visual Studio preset once:

```bash
bash src/cpp/scripts/windows/msbuild-generate-sln.sh
```

This configures the `windows-msbuild` preset and generates a solution under:

```text
src/cpp/out/obj/windows-msbuild/*.sln
```

To generate and open it immediately:

```bash
bash src/cpp/scripts/windows/msbuild-generate-sln.sh --open
```

### Windows dual coverage workflow

Windows now has two report lanes so you can compare Microsoft native coverage against OpenCppCoverage:

```bash
# Microsoft native coverage
bash src/cpp/scripts/windows/ninja-msvc-coverage-build.sh
bash src/cpp/scripts/windows/ninja-msvc-coverage-run.sh
bash src/cpp/scripts/windows/ninja-msvc-coverage-report.sh

# OpenCppCoverage tool-native HTML
bash src/cpp/scripts/windows/ninja-msvc-opencppcoverage-run.sh
```

`ninja-msvc-opencppcoverage-run.sh` reuses the Windows coverage test binary and emits tool-native HTML plus Cobertura and binary exports. Set `KOG_OPENCPPCOVERAGE_EXE` if the tool is installed outside the default path.

### CLI launcher scripts (alternative)

```bash
# Add scripts/ (bash launchers) to PATH, then:
kano-git commit
kog commit    # short alias
kog amend     # amend previous commit (native)

# enhance native git config completion for kano keys (keeps git completion)
source <(kano-git completion git-bash)
```

### Launcher mode (developer vs installed package)

- Default behavior (no marker): launcher assumes **developer mode**.
- In manual developer runs, strict native-only mode is enforced: if the C++ binary is missing, the launcher tells you to run `./kog self build` (or `./kog self rebuild`) instead of auto-building implicitly.
- Installed-package behavior: if marker file exists, launcher assumes **packaged install mode** and will **not** auto-build.
- Marker path:
  - default: `.kano-installed-marker` at repo root
  - override: `KANO_GIT_INSTALL_MARKER=/absolute/path/to/marker`
- In packaged-install mode, if binary is missing, launcher prints an installation-corruption warning and asks user to reinstall.

### Launcher progress and update checks

- Long-running launcher tasks (prerequisite/build/package update command) emit heartbeat logs by default:
  - `[launcher] <task> still running... <seconds>s elapsed`
- Heartbeat controls:
  - `KANO_GIT_PROGRESS_HEARTBEAT=0` to disable
  - `KANO_GIT_PROGRESS_INTERVAL_SECONDS=<n>` to adjust interval (default: `10`)

- Developer mode update check:
  - launcher fetches remote (`upstream` preferred, else `origin`) and checks whether remote default branch is ahead of local `HEAD`
  - when updates exist, launcher prompts whether to run sync workflow (`smart-sync-origin-latest.sh`)

- Packaged mode update check (interval-based, default every 6h):
  - `KANO_GIT_PACKAGE_VERSION_CHECK_CMD` command that prints latest available version (single line)
  - `KANO_GIT_PACKAGE_UPDATE_CMD` command used to perform update when user confirms
  - `KANO_GIT_PACKAGE_UPDATE_CHECK_INTERVAL_SECONDS=<n>` check interval in seconds (default: `21600`)

- Global toggle:
  - `KANO_GIT_AUTO_UPDATE_CHECK=0` disables launcher update checks

- Rebuild trigger (developer mode):
  - use `./kog self build` for an incremental rebuild
  - use `./kog self rebuild` for a clean rebuild

## Command Tree

| Command | Description |
|---------|-------------|
| `commit` | AI-powered commit message generation |
| `amend` | Amend previous commit or combine unpushed local commits |
| `cache` | Show/clear kano-git cache with system/global/local effective view |
| `resolve` | AI-powered conflict resolution |
| `sync` | Repository synchronization (origin-latest, upstream, stable-dev) |
| `push` | Multi-remote push workflow |
| `worktree` | Git worktree management (create, list, remove, sync) |
| `subtree` | Git subtree operations (add, pull, push, split, list) |
| `submodule` | Enhanced submodule management |
| `scalar` | Git Scalar mono-repo performance |
| `p4` | Git-Perforce bridge |
| `svn` | Git-Subversion bridge |
| `branch` | Branch operations (rebase-upstream, compare, cherry-pick) |
| `workspace` | Multi-repo workspace operations |
| `clone` | Clone with upstream support |
| `doctor` | Environment and repo health checks |
| `version` | Show version |

### Native workspace discovery and wave planning

The C++ CLI now includes a native discovery/cache/scheduler core for workspace planning:

```bash
# Native-first discovery (JSON repo list)
kano-git workspace discover --native-metadata-level full

# Deterministic execution waves with cycle status
kano-git workspace discover --emit-waves

# Native-first workspace status (table/json/markdown)
kano-git workspace status --format table
kano-git workspace status --format json

# Native plan output for workspace update (no execution)
kano-git workspace update --native-plan-only

# Shell fallback escape hatch
kano-git workspace status --shell
kano-git workspace foreach --shell --command "git status --porcelain"
```

Notes:
- Native discovery cache payload keeps parity fields (`version`, `generated_epoch`, `gitmodules_mtime`, `marker`, `repos`).
- Wave output is deterministic and exits with code `2` when cycle nodes are detected.
- Shared planner schema for `update` and `foreach` is documented in `docs/design/workspace-native-planner-contract.md`.

### Module boundary convention (kano_git_core)

Boundary hardening rule used in `kano_git_core`:

- One domain directory = one facade module entrypoint (for example `private/workspace/native_workspace.hpp/.cpp`).
- Domain internals stay behind the facade (`discovery.*`, `scheduler.*` are internal to workspace domain).
- Command adapters depend on facade only (for example `workspace_cmd.cpp` includes `workspace/native_workspace.hpp` only).

About `cppm`:

- Your model is directionally correct: a directory-level module can have one primary `*.cppm` interface.
- In practice, one directory can still have multiple internal `*.cpp` units behind that single interface.
- Prefer one exported interface module per domain, then use implementation units/partitions as needed.

## Architecture

```
src/
├── cpp/
│   ├── code/systems/kano_git_core/    # shared native domain logic
│   ├── code/systems/kano_git_command/ # command/product/runtime layers
│   ├── code/systems/kano_git_test_core/ # shared native test helpers
│   ├── code/apps/kano_git_cli/        # CLI frontend
│   ├── code/apps/kano_git_tui/        # TUI frontend
│   ├── code/tests/                    # native test targets + runners
│   ├── scripts/                       # canonical native build/test/coverage scripts
│   └── out/                           # native artifacts by preset/config
└── shell/                             # support, docs automation, acceptance tests
```

The native CLI/TUI are no longer just thin shells over script logic.

- core workspace, planner, status, commit, and runtime behavior now lives in native C++ systems
- shell under `src/shell/` remains for support helpers, docs automation, wrappers, and acceptance flows
- `kano_git_core` and sibling systems are designed for reuse across CLI, TUI, and future frontends

## Project Structure

```
src/
├── cpp/
│   ├── CMakeLists.txt          # Build configuration (C++20 modules)
│   ├── CMakePresets.json       # Multi-config presets (Debug/Release)
│   ├── vcpkg.json              # Native package dependencies
│   ├── scripts/                # Host/preset build wrappers
│   ├── code/systems/kano_git_core/  # Core static library
│   ├── code/systems/kano_git_command/ # Native command/product/runtime layers
│   ├── code/systems/kano_git_test_core/ # Shared native test helpers
│   ├── code/apps/kano_git_cli/      # CLI executable
│   ├── code/apps/kano_git_tui/      # TUI executable
│   ├── code/tests/                  # Native test targets + runners
│   ├── out/bin/                # Final executables by preset/config
│   ├── out/lib/                # Libraries by preset/config
│   ├── out/obj/                # CMake/Ninja/MSBuild intermediates
│   └── out/coverage/           # Coverage artifacts and reports
└── shell/                      # Support helpers / docs / acceptance
    ├── support/
    ├── docs/
    └── test/

scripts/                        # Bash launchers
├── kano-git / kano-git.bat
└── kog / kog.bat
```

### Third-party dependency policy

- Ordinary upstream C++ libraries should be fetched through CMake `FetchContent` and live under the build `_deps` area.
- `src/cpp/code/thirdparty/` is reserved for dependencies we intentionally fork or customize in-repo.
- `CLI11`, `FTXUI`, `Catch2`, and `tomlplusplus` all follow the fetch-first model unless a future Kano-specific fork requires a vendored submodule path.
