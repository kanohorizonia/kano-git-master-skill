# Profiling Area

This directory contains curated build-feature profiling matrices that measure the effect of individual build options on compile and link times.

## Quick Start

> **Note:** Shipped matrices use Windows (`windows-ninja-msvc`) presets by default. For Linux or macOS, create a platform-specific matrix JSON or override presets per run. See [Cross-Platform Matrices](#cross-platform-matrices) below.

```bash
# Run the default matrix (launcher + unity + PGO scenarios)
bash src/cpp/shared/infra/scripts/stages/profile.sh default

# Render the profile report
bash src/cpp/shared/infra/scripts/stages/profile-report.sh default

# Other shipped matrices
bash src/cpp/shared/infra/scripts/stages/profile.sh launchers    # none vs auto vs ccache vs sccache
bash src/cpp/shared/infra/scripts/stages/profile.sh unity       # off vs full vs changed
bash src/cpp/shared/infra/scripts/stages/profile.sh pgo         # baseline vs PGO use
```

## Report Output

Profile reports are written to `docs/profiling/<slug>/`:

```
docs/profiling/
├── local-build-profile/   # default matrix output
│   ├── profile.json       # canonical structured artifact
│   ├── summary.md         # Markdown projection
│   └── index.html         # HTML projection
├── launcher-profile/      # launchers matrix output
├── unity-profile/         # unity matrix output
└── pgo-profile/           # pgo matrix output
```

`profile.json` is the canonical artifact — `summary.md` and `index.html` are projections for human reading.

## Artifact Locations

| Kind | Path |
|------|------|
| Per-case raw output | `.kano/tmp/profiling/<matrix>/<case>/` |
| Merged profile JSON | `.kano/tmp/profiling/<matrix>/profile.json` |
| Report output | `docs/profiling/<slug>/` |

## Matrix JSON Format

Each matrix is a JSON file under `src/cpp/shared/infra/scripts/profiling/matrices/`:

```json
{
  "name": "my-matrix",
  "reportSlug": "my-profile",
  "defaults": {
    "configurePreset": "windows-ninja-msvc",
    "buildPreset": "windows-ninja-msvc-release",
    "buildDir": "src/cpp/out/obj/windows-ninja-msvc",
    "config": "Release",
    "launcher": "auto",
    "unity": "off",
    "modules": "off",
    "pgo": "off",
    "workflow": "baseline"
  },
  "cases": [
    { "id": "my-case", "launcher": "sccache", "unity": "changed" }
  ]
}
```

### Top-level fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Matrix identifier; used as directory name under `.kano/tmp/profiling/` |
| `reportSlug` | No | Report directory name; defaults to `name` |
| `defaults` | Yes | Default values for all cases |
| `cases` | Yes | Array of case objects |

### Defaults fields

| Field | Description |
|-------|-------------|
| `configurePreset` | CMake configure preset (e.g., `windows-ninja-msvc`, `linux-ninja-gcc`) |
| `buildPreset` | CMake build preset (e.g., `windows-ninja-msvc-release`) |
| `buildDir` | CMake binary directory (must match preset) |
| `config` | Build configuration (`Debug` or `Release`) |
| `launcher` | Compiler launcher: `none`, `auto`, `ccache`, `sccache` |
| `unity` | Unity build: `off`, `full`, `changed` |
| `modules` | C++20 modules: `on` or `off` |
| `pgo` | PGO mode: `off`, `collect`, `use` |
| `workflow` | `baseline` (measure_iteration_baseline.sh) or `pgo` (pgo-rebuild.sh) |

### Case fields

Any field in a case overrides the corresponding default. Common overrides:

| Field | Values | Description |
|-------|--------|-------------|
| `id` | string | Case identifier; used as subdirectory name |
| `launcher` | `none`, `auto`, `ccache`, `sccache` | Compiler launcher |
| `unity` | `off`, `full`, `changed` | Unity build mode |
| `modules` | `on`, `off` | C++20 modules |
| `pgo` | `off`, `collect`, `use` | PGO mode |
| `coverage` | `on`, `off` | Coverage instrumentation |
| `workflow` | `baseline`, `pgo` | Which workflow script to run |
| `configurePreset` | string | Override configure preset |
| `buildPreset` | string | Override build preset |

## Adding a New Matrix

1. Create `src/cpp/shared/infra/scripts/profiling/matrices/<name>.json`
2. Run `bash src/cpp/shared/infra/scripts/stages/profile.sh <name>`
3. Render: `bash src/cpp/shared/infra/scripts/stages/profile-report.sh <name>`
4. Commit the matrix JSON; profile reports (`docs/profiling/<slug>/`) are generated artifacts and should also be committed

## Cross-Platform Matrices

The same matrix JSON can be reused across platforms by setting platform-specific presets in `defaults`. For a Linux-only smoke test, create a matrix with Linux presets:

```json
{
  "name": "linux-smoke",
  "reportSlug": "linux-smoke-profile",
  "defaults": {
    "configurePreset": "linux-ninja-gcc",
    "buildPreset": "linux-ninja-gcc-release",
    "buildDir": "src/cpp/out/obj/linux-ninja-gcc",
    "config": "Release",
    "launcher": "none",
    "unity": "off",
    "modules": "off",
    "pgo": "off",
    "workflow": "baseline"
  },
  "cases": [
    { "id": "baseline" }
  ]
}
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `KOG_PROFILE_TMP_ROOT` | `.kano/tmp/profiling` | Raw per-case and merged profile root |
| `KOG_PROFILE_REPORT_ROOT` | `docs/profiling` | Final report output directory |
| `KOG_CMAKE_CACHE_ARGS_JSON` | (none) | JSON object of cache variable overrides |

## Workflow Scripts

| Script | Purpose |
|--------|---------|
| `profiling/run.sh` | Run a profiling matrix |
| `profiling/report.sh` | Render profile reports from matrix run |
| `stages/profile.sh` | Alias to `profiling/run.sh` |
| `stages/profile-report.sh` | Alias to `profiling/report.sh` |
| `common/measure_iteration_baseline.sh` | Baseline build iteration measurer |
| `workflows/pgo-rebuild.sh` | PGO collect → use workflow |
