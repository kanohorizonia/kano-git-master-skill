# Pixi Development Environment

`pixi` is the repo-level environment and task runner for `kano-git-master-skill`.

It does not replace `vcpkg`.

- `pixi` manages cross-platform developer tools such as `bash`, `cmake`, `ninja`, `git`, `gh`, `jq`, `python`, and `ripgrep`
- `vcpkg` continues to manage native C++ libraries declared in `src/cpp/vcpkg.json`
- host-specific system prerequisites such as Visual Studio Build Tools still stay in the existing prerequisite scripts

## Install pixi

Follow the official installation instructions from `https://pixi.sh`, then run from the repo root:

```bash
pixi install
pixi run env-summary
```

## Current task model

The initial integration keeps the existing build scripts as the source of truth.

```bash
# Show tool versions from the pixi environment
pixi run env-summary

# Run host-specific bootstrap scripts
pixi run bootstrap-windows-system
pixi run bootstrap-linux-system
pixi run bootstrap-macos-system

# Build the native CLI for the current host platform
pixi run build

# Run repo tests
pixi run quick-test
pixi run full-test
```

## Why vcpkg stays

The native CLI still relies on the CMake + preset + `VCPKG_ROOT` flow documented under `src/cpp/`.

That means the current contract is:

1. `pixi` provides a reproducible tool environment
2. platform prerequisite scripts fill system gaps that pixi should not own
3. `vcpkg` resolves native library dependencies for the C++ build

## Prerequisite behavior by platform

The prerequisite scripts are now pixi-aware across platforms for the common CLI/build tools that pixi is meant to own.

- Windows: if `cmake`, `ninja`, or `python` are already available from the active pixi environment, the script skips their `winget` install/upgrade steps
- Linux: if `cmake`, `ninja`, `git`, or `python` are already available from the active pixi environment, the script skips those `apt-get` packages but still installs the compiler/runtime packages it owns
- macOS: if `cmake`, `ninja`, `git`, or `python` are already available from the active pixi environment, the script skips those `brew` packages but still keeps the host toolchain flow intact

System compiler/toolchain ownership still stays outside pixi:

- Windows: Visual Studio Build Tools remain a system prerequisite
- Linux: compiler packages such as `gcc-15`, `g++-15`, `clang`, and `pkg-config` remain in the apt-based bootstrap path
- macOS: Xcode Command Line Tools remain required, and the script still keeps the Homebrew LLVM path available

## Task naming notes

- `pixi run build` is the host-default native build entrypoint
- `pixi run build-release` is the host-default release build entrypoint
- `pixi run build-dev`, `build-dev-linux`, and `build-dev-macos` remain explicit helper tasks for skill development workflows

## Next integration targets

- keep launcher/self-build and pixi task names aligned when new native build flows are added
- add CI steps that run `pixi install --locked` before native build/test entrypoints
