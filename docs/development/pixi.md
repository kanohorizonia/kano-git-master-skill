# Pixi Development Environment

This repo follows the **shared Pixi environment contract** defined in `kano-shell-master-skill/SKILL.md` under "Environment Activation via Pixi (PATH-First Pattern)".

`pixi` is split into two layers for `kano-git-master-skill`.

- `src/cpp/shared/infra/pixi.toml` is the canonical shared manifest for common native build/test/report/bootstrap flows
- repo-root `pixi.toml` is reserved for repo-specific extension tasks such as shell acceptance and docs flows

It does not replace `vcpkg`.

- `pixi` manages cross-platform developer tools such as `bash`, `cmake`, `ninja`, `git`, `gh`, `jq`, `python`, and `ripgrep`
- `vcpkg` continues to manage native C++ libraries declared in `src/cpp/vcpkg.json`
- host-specific system prerequisites such as Visual Studio Build Tools still stay in the existing prerequisite scripts

## Shared Pixi Bootstrap Contract

### For Local Workflows (PRIMARY)

Use `pixi run <task>` for reproducible environment setup:

```bash
pixi install --manifest-path src/cpp/shared/infra/pixi.toml
pixi run --manifest-path src/cpp/shared/infra/pixi.toml env-summary
pixi run --manifest-path src/cpp/shared/infra/pixi.toml build
pixi run --manifest-path src/cpp/shared/infra/pixi.toml quick-test
```

Or run from inside the shared infra directory:

```bash
cd src/cpp/shared/infra
pixi install
pixi run env-summary
pixi run build
pixi run quick-test
```

### For Direct Script Execution

Core scripts under `src/cpp/shared/infra/scripts/` are PATH-first and should activate the Pixi environment via shared bootstrap:

```bash
# Direct execution should resolve tools the same way as `pixi run --manifest-path src/cpp/shared/infra/pixi.toml`
bash src/cpp/shared/infra/scripts/self/build.sh
bash src/cpp/shared/infra/scripts/stages/test.sh
```

**The reusable bootstrap pattern** (adoption point):

Scripts that run build tools directly should source `src/cpp/shared/infra/scripts/lib/pixi_bootstrap.sh` and call its activation function:

```bash
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Bootstrap pixi environment if not already active
source "$SCRIPT_DIR/pixi_bootstrap.sh"
kano_pixi_bootstrap_activate

# Your script logic here...
```

This pattern is already adopted in:
- `src/cpp/shared/infra/scripts/lib/unix_preset_build.sh`
- `src/cpp/shared/infra/scripts/lib/windows_preset_build.sh`

**Do NOT:**
- Hardcode `.pixi/envs/...` paths in leaf scripts
- Fall back silently to random system tools
- Invoke `pixi install` from core scripts (only top-level wrappers do this)

**DO:**
- Activate Pixi environment via shared bootstrap (if not already active)
- Fail fast if required tools are missing
- Log which environment is active for debugging

## Install pixi

Follow the official installation instructions from `https://pixi.sh`, then run either from `src/cpp/shared/infra/` or with an explicit manifest path:

```bash
pixi install --manifest-path src/cpp/shared/infra/pixi.toml
pixi run --manifest-path src/cpp/shared/infra/pixi.toml env-summary
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

- `pixi run --manifest-path src/cpp/shared/infra/pixi.toml build` is the host-default native build entrypoint
- `pixi run --manifest-path src/cpp/shared/infra/pixi.toml build-release` is the host-default release build entrypoint
- `pixi run docs`, `pixi run docs-ci`, and shell acceptance tasks remain repo-root extensions because they are specific to this skill repo

## Next integration targets

- keep launcher/self-build and pixi task names aligned when new native build flows are added
- add CI steps that run `pixi install --locked` before native build/test entrypoints
