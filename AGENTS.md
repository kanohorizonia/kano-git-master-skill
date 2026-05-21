# AGENTS.md

## Temporary test artifacts

- Put all smoke-test outputs, local verification outputs, scratch build outputs, and ad-hoc debug artifacts under `.kano/tmp/`.
- Do not create repo-root directories like `_local-ci-smoke/`, `_local-msvc-debug/`, `_local-msvc-smoke/`, or similar one-off test folders.
- If a test needs multiple subdirectories, nest them under `.kano/tmp/<purpose>/`.
- Prefer names that make cleanup obvious, for example:
  - `.kano/tmp/local-ci-smoke/`
  - `.kano/tmp/msvc-debug/`
  - `.kano/tmp/opencpp-smoke/`
- Before finishing work, clean up temporary test artifacts that are no longer needed.

## Ignore-gate expectations

- Temporary verification artifacts must not be staged for commit.
- If a local test needs files that should survive briefly between runs, keep them in `.kano/tmp/` so ignore rules and safety checks can treat them as workspace-local temporary state.

## Build toolchain ownership (Windows self-build)

- Treat `pixi.toml` as the source of truth for repo-local build tools such as `cmake`, `ninja`, `git`, `bash`, and `python`.
- Treat Windows system prerequisites as a separate layer: Visual Studio / `vcvarsall.bat`, PowerShell, and other host-only tooling that pixi does not own.
- When `kog self build` or Windows preset builds fail because of `cmake` / `ninja` resolution, fix the pixi/build-entrypoint layer first.
- Do **not** patch this class of problem by hardcoding user-specific paths, adding ad-hoc launcher PATH hacks, or preferring random system installations over the repo `.pixi` environment.
- On Windows, verify the actual execution path before editing: launcher `self build` → pixi task / native build entrypoint → PowerShell preset helper → CMake preset.
- If the observed failure involves tool selection, confirm whether the active command is running inside the pixi environment before changing launcher shell shims or prerequisite scripts.
- **Build Command Prioritization**: When building the workspace or the CLI itself, prioritize build tools and commands in the following order:
  1. **Primary Entrypoint**: `kog self build` (e.g. `./scripts/kog self build` or `scripts\kog.bat self build`). This is the preferred, top-level launcher entrypoint that bootstraps the environment and verifies prerequisite states.
  2. **Secondary Entrypoint**: `pixi run <build-task>` (e.g. `pixi run build` or using the shared manifest `pixi run --manifest-path src/cpp/shared/infra/pixi.toml build`).
  3. **Fallback Entrypoint**: Direct `cmake` / `ninja` commands. Use these only for low-level debugging or when custom options are strictly necessary. However, **any custom/direct `cmake` build command MUST also be registered/written as a task inside `pixi.toml`** (specifically `src/cpp/shared/infra/pixi.toml` or `pixi.toml` under the target's platform tasks). This maintains `pixi.toml` as the single source of truth for all build tool chains.


## Pixi environment activation and launcher responsibilities

This repo follows the **shared Pixi environment contract** defined in `kano-shell-master-skill/SKILL.md` (see "Environment Activation via Pixi" section).

**Summary:**
- Prefer `pixi run <task>` as the primary entrypoint for reproducible local workflows.
- Keep core scripts (under `src/cpp/shared/infra/scripts/`) PATH-first; they should resolve tools via normal command lookup, not hardcoded `.pixi` paths.
- Top-level wrappers (like `scripts/kog`, `scripts/kano-git`, or CI entry points) may run explicit Pixi preflight checks or install commands, but should avoid silently installing tools.
- Core scripts should activate the Pixi environment (via shared bootstrap) and fail fast if required tools are missing—they should never silently invoke `pixi install`.
- Direct script execution (e.g., `bash scripts/self/build.sh`) must reuse the same environment and tool resolution as `pixi run build` via the shared bootstrap; if the bootstrap is not present, fail fast rather than silently fall back.

For full details on Pixi bootstrap patterns and idempotency rules, see `kano-shell-master-skill/SKILL.md`.
