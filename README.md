# Git Master Skill

**Version**: 0.1.0-beta
**Status**: Beta Release

Advanced Git automation for multi-repository workspaces with a native `kog` / `kano-git` command surface.

## Quick Start

```bash
# Show skill version
./scripts/internal/show-version.sh

# Get started
cat docs/README.md
```

## Native C++ Build (Required Method)

When building native `kano-git` / `kog`, use the stable entrypoints under `src/cpp/shared/infra/scripts/`.

```bash
# Default one-shot build / rebuild
bash src/cpp/shared/infra/scripts/self/build.sh
bash src/cpp/shared/infra/scripts/self/rebuild.sh

# Atomic stages
bash src/cpp/shared/infra/scripts/self/build.sh
bash src/cpp/shared/infra/scripts/stages/test.sh
bash src/cpp/shared/infra/scripts/stages/test-report.sh
bash src/cpp/shared/infra/scripts/stages/coverage-build.sh
bash src/cpp/shared/infra/scripts/stages/coverage-gather.sh
bash src/cpp/shared/infra/scripts/stages/coverage-report.sh

# Composed workflows
bash src/cpp/shared/infra/scripts/workflows/coverage-all.sh
bash src/cpp/shared/infra/scripts/workflows/pgo-rebuild.sh

# Profiling matrices + profile report
bash src/cpp/shared/infra/scripts/stages/profile.sh default
bash src/cpp/shared/infra/scripts/stages/profile-report.sh default
```

Do not use ad-hoc direct CMake/Ninja command sequences in this repo unless a maintainer explicitly asks for it.

### Native Script Layers

- `src/cpp/shared/infra/scripts/` — canonical shared/base infra, entrypoints, toolchain helpers, metadata
- `src/cpp/shared/infra/scripts/self/` — stable one-shot entrypoints (`build`, `rebuild`)
- `src/cpp/shared/infra/scripts/stages/` — atomic stage entrypoints (`test`, `coverage-*`, `profile*`)
- `src/cpp/shared/infra/scripts/workflows/` — composed workflows such as coverage-all and pgo-rebuild
- `src/cpp/shared/infra/scripts/profiling/` — profiling matrices, matrix runner, and profile-report rendering
- `src/cpp/shared/infra/scripts/platform/` — platform-specific leaf wrappers

### Report Kinds

Native reporting is now modeled explicitly as two report kinds:

- `test` — test-result rendering / static test report output
- `coverage` — coverage collection / rendering output

Older `*-kano-report.sh` scripts remain as compatibility wrappers but should be treated as legacy names.

### Profiling Area

The profiling area lives under `src/cpp/shared/infra/scripts/profiling/` and is designed for curated feature-combination runs such as:

- compiler launcher comparisons (`none`, `ccache`, `sccache`, `auto`)
- unity build comparisons (`off`, `full`, `changed`)
- PGO workflow comparisons (`baseline` vs `pgo`)
- mixed feature scenarios such as launcher + unity

Shipped profiling matrices:

- `src/cpp/shared/infra/scripts/profiling/matrices/default.json`
- `src/cpp/shared/infra/scripts/profiling/matrices/launchers.json`
- `src/cpp/shared/infra/scripts/profiling/matrices/unity.json`
- `src/cpp/shared/infra/scripts/profiling/matrices/pgo.json`

Entry points:

```bash
# Run a profiling matrix
bash src/cpp/shared/infra/scripts/stages/profile.sh default

# Render the JSON-first profile report
bash src/cpp/shared/infra/scripts/stages/profile-report.sh default
```

Artifacts are written under:

- raw per-case outputs: `.kano/tmp/profiling/<matrix>/<case>/`
- merged report outputs: `docs/profiling/<slug>/`

Each profile report emits:

- `profile.json` — canonical structured artifact
- `summary.md` — Markdown projection
- `index.html` — lightweight HTML projection

## Features

- **Repository Initialization Workflow** - Automated repository setup with multi-remote, orphan branches, and submodules
- **Version Information** - Extract version info from git, git-p4, git-svn
- **Worktree Management** - Manage multiple working trees efficiently
- **Subtree Management** - Include external repositories as subtrees
- **Submodule Enhancement** - Enhanced submodule operations with multi-URL support
- **Mono-repo Optimization** - Git Scalar for large repositories (10-20x faster)
- **VCS Bridges** - Integrate with Perforce (git-p4) and Subversion (git-svn)

## Documentation

All documentation is in the `docs/` directory:

- [Documentation Index](./docs/README.md) - Start here
- [Quick Start Guide](./docs/guides/quick-start.md)
- [CPA Commit Plan Workflow](./docs/guides/cpa-commit-plan-workflow.md) - Full-auto/semi-auto AI commit flows with stage diagram
- [Repository Initialization Workflow Examples](./docs/examples/repo-initialization-workflow-examples.md) - Complete usage examples
- [Submodule Guide](./docs/guides/submodule.md) - Enhanced submodule management
- [Common Pitfalls](./docs/guides/common-pitfalls.md) - Troubleshooting guide
- [Changelog](./docs/status/changelog.md)

## Native Commit Semantics

`kog commit` is plan-first.

- `kog commit -m "..."` synthesizes a minimal transient commit plan, then reuses the
  same plan-backed validation/apply path as `--plan-file`
- `kog commit --plan-file <file>` applies an explicit prepared plan
- `kog commit --agent <name> -m "..."` uses the same synthesized-plan path with
  agent proxy rules
- `kog commit --plan-file <file> -m "..."` is invalid and fails explicitly

## Wrapper Entry Points

- keep using `scripts/kano-git` and `scripts/kog` as the canonical wrapper names
- on Windows CMD / PowerShell, the paired `scripts/kano-git.bat` and `scripts/kog.bat`
  are provided so those shells can invoke the launcher without a shell-selection prompt
- installer behavior is owned by native `kog` commands; separate installer wrapper names
  are no longer kept in `scripts/`

## Installation

No installation required. Clone and use:

```bash
git clone <repo-url>
cd kano-git-master-skill

# Run any script
./scripts/worktree/create-worktree.sh --help
```

### External Workspace Discovery

Native workspace commands such as `kog status` can merge additional repo roots
outside the current workspace through layered `kog_config.toml` settings.

The shipped default enables nearby skill discovery under `~/.agents/skills`:

```toml
[workspace.external]
roots = ["~/.agents/skills"]
inherit = true
```

You can inspect or override these settings with:

```bash
kog config workspace.external.roots
kog config --help workspace.external.roots
```

When external repos are merged into `kog status`, their type now keeps both the
external scope and the repo role when possible:

- `external-root` for the top-level repo discovered from an external root
- `external-registered` for repos registered by that external root repo via `.gitmodules`
- `external-unregistered` for direct child repos that are not registered there

Plain `external` is no longer emitted. In this naming, `root` refers to the
matched external discovery root repo, not the current workspace root.

When an external root is a container directory such as `~/.agents/skills`,
`kog status` first discovers child repo roots such as `~/.agents/skills/kano`,
then merges that repo plus its registered repos.

## Requirements

- Git 2.x or higher
- Bash 4.x or higher (or Git Bash on Windows)
- Git Scalar (optional, for mono-repo optimization)

## Project Structure

```
kano-git-master-skill/
├── VERSION                     # Version number (single source of truth)
├── SKILL.md                    # Skill documentation
├── docs/                       # User documentation
├── pixi.toml                   # Repo-local tool/task environment
├── src/
│   ├── cpp/                    # Native product/build root
│   │   ├── CMakeLists.txt
│   │   ├── CMakePresets.json
│   │   ├── vcpkg.json
│   │   ├── code/              # systems, apps, tests
│   │   ├── shared/infra/scripts/ # Canonical native self/stage/workflow/report scripts
│   │   └── out/               # Native artifacts
│   └── shell/                 # Support helpers, docs automation, acceptance tests
└── scripts/                    # Thin launchers and shell automation
    ├── internal/           # Skill maintenance scripts
    │   ├── show-version.sh # Show skill version
    │   ├── bump-version.sh # Bump skill version
    │   ├── create-tag.sh   # Create git tag
    │   └── init-kano-dev-skill.sh  # Initialize Kano skill repos for development
    ├── tags/               # Tag management
    │   └── list-tags.sh    # List git tags
    ├── core/               # Core operations
    │   ├── init-repo-workflow.sh      # Complete initialization workflow
    │   ├── setup-multi-remote.sh      # Multi-remote configuration
    │   ├── create-orphan-branch.sh    # Orphan branch creation
    │   ├── init-empty-repo.sh         # Initialize empty repository
    │   ├── smart-clone.sh             # Clone with upstream remote
    │   └── update-repo.sh             # Update repository
    ├── worktree/           # Worktree management
    ├── subtree/            # Subtree management
    ├── submodules/         # Submodule operations
    │   ├── smart-submodule.sh         # Canonical submodule entrypoint
    │   ├── kog-submodule.sh           # Enhanced multi-remote submodule ops
    │   ├── add-submodule.sh           # Compatibility wrapper (add)
    │   ├── remove-submodule.sh        # Remove submodule
    │   ├── update-submodules.sh       # Update all submodules
    │   ├── sync-urls.sh               # Sync submodule URLs
    │   └── submodule-common.sh        # Shared submodule helpers
    ├── mono-repo/          # Mono-repo optimization
    ├── vcs-bridges/        # VCS integration (P4, SVN)
    └── lib/                # Helper libraries
```

Current architecture rule:

- native product behavior lives under `src/cpp/`
- root `scripts/` remain launchers, compatibility entrypoints, and shell automation
- when docs and older shell-era assumptions disagree, prefer `src/cpp/`, `src/cpp/shared/infra/scripts/`, `src/cpp/out/`, and the current native command implementation

Current native script contract:

- prefer `src/cpp/shared/infra/scripts/self/*` for default build / rebuild entrypoints
- prefer `src/cpp/shared/infra/scripts/stages/*` for atomic stage automation
- prefer `src/cpp/shared/infra/scripts/workflows/*` for composed flows
- prefer explicit report kinds: `test-report` and `coverage-report`

## Usage Examples

### Repository Initialization Workflow

Initialize a new repository with multi-remote setup, orphan branch, and submodules:

```bash
# Complete workflow with all features
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-http-url https://github.com/myuser/myproject.git \
  --upstream-ssh git@github.com:upstream/myproject.git \
  --upstream-http https://github.com/upstream/myproject.git \
  --repo-dir ./myproject \
  --orphan-branch dev/tools \
  --submodule "git@github.com:myuser/tool1.git:tools/tool1"

# Simple initialization
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --repo-dir ./myproject

# Preview with dry-run
./scripts/core/init-repo-workflow.sh \
  --repo-url git@github.com:myuser/myproject.git \
  --dry-run
```

**Kano Skill Development Initialization** - Internal tool for setting up Kano skill repositories:

```bash
# Initialize kano-agent-skill with development skills
# Tooling branch will be auto-named: dev/kano-agent-skill-tooling
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir .agents/kano \
  --skill "git@github.com:user/skill1.git:https://github.com/user/skill1.git:skills/skill1" \
  --skill "git@github.com:user/skill2.git:https://github.com/user/skill2.git:skills/skill2"

# Preview with dry-run
./scripts/internal/init-kano-dev-skill.sh \
  --repo-ssh git@github.com:dorgonman/kano-agent-skill.git \
  --repo-https https://github.com/dorgonman/kano-agent-skill.git \
  --repo-dir .agents/kano \
  --dry-run
```

See [Workflow Examples](./docs/examples/repo-initialization-workflow-examples.md) and [Kano Dev Skill Init Examples](./docs/examples/init-kano-dev-skill-example.md) for more usage patterns.

### Multi-Remote Configuration

Setup multiple remotes with SSH/HTTPS fallback:

```bash
cd myproject
./scripts/core/setup-multi-remote.sh \
  --origin-ssh git@github.com:myuser/myproject.git \
  --origin-http https://github.com/myuser/myproject.git \
  --upstream-ssh git@github.com:upstream/myproject.git \
  --upstream-http https://github.com/upstream/myproject.git
```

### Orphan Branch Management

Create isolated orphan branches for development tools:

```bash
# Create orphan branch
./scripts/core/create-orphan-branch.sh \
  --branch dev/tools \
  --push

# Create and return to original branch
./scripts/core/create-orphan-branch.sh \
  --branch dev/docs \
  --return
```

### Enhanced Submodule Management

Manage submodules with multi-URL support and automatic fallback:

```bash
# Canonical entrypoint (recommended)
./scripts/submodules/smart-submodule.sh add \
  --path tools/formatter \
  --remote origin \
    --ssh git@github.com:myuser/formatter.git \
    --https https://github.com/myuser/formatter.git \
  --remote upstream \
    --ssh git@github.com:original/formatter.git \
    --https https://github.com/original/formatter.git

# Equivalent direct command
# Add submodule with multiple remotes
./scripts/submodules/kog-submodule.sh add \
  --path tools/formatter \
  --remote origin \
    --ssh git@github.com:myuser/formatter.git \
    --https https://github.com/myuser/formatter.git \
  --remote upstream \
    --ssh git@github.com:original/formatter.git \
    --https https://github.com/original/formatter.git

# Sync remotes/branch alignment
./scripts/submodules/smart-submodule.sh sync

# Update submodule checkouts
./scripts/submodules/smart-submodule.sh update --remote --recursive
```

### Show Skill Version

```bash
# Show full version info
./scripts/internal/show-version.sh

# Show version number only
./scripts/internal/show-version.sh --short
```

### Manage Skill Version

```bash
# Bump patch version (0.1.0 -> 0.1.1)
./scripts/internal/bump-version.sh patch

# Bump minor version (0.1.0 -> 0.2.0)
./scripts/internal/bump-version.sh minor

# Remove pre-release label (0.1.0-beta -> 0.1.0)
./scripts/internal/bump-version.sh patch --remove-pre-release

# Bump and create tag
./scripts/internal/bump-version.sh minor --create-tag
```

### Manage Tags

```bash
# Create tag from VERSION file
./scripts/internal/create-tag.sh --from-version-file -m "Beta release"

# List all tags
./scripts/tags/list-tags.sh

# Show latest tag
./scripts/tags/list-tags.sh --latest

# List tags with details
./scripts/tags/list-tags.sh --detailed
```

### Worktree Management

```bash
# Create worktree for a branch
./scripts/worktree/create-worktree.sh feature-branch

# List all worktrees
./scripts/worktree/list-worktrees.sh
```

### Mono-repo Optimization

```bash
# Register with Git Scalar (10-20x faster)
./scripts/mono-repo/scalar/register.sh

# Check status
./scripts/mono-repo/scalar/status.sh
```

### VCS Bridges

```bash
# Clone from Perforce
./scripts/vcs-bridges/p4/clone.sh //depot/project/...

# Clone from Subversion
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo
```

## Beta Release

This is a beta release. All core features are implemented and functional, but:

- Test coverage is incomplete for some features
- Performance benchmarks not yet available
- Some edge cases may not be fully handled

**Feedback welcome!** Please report issues or suggestions.

## Troubleshooting

### Common Issues

**Branch Already Exists**
```bash
# Use different branch name or force overwrite
./scripts/core/create-orphan-branch.sh --branch dev/tools-v2
# OR (DANGEROUS)
./scripts/core/create-orphan-branch.sh --branch dev/tools --force-overwrite-branch
```

**SSH Authentication Failure**
```bash
# System automatically falls back to HTTPS if provided
# To fix SSH: check and add SSH key
ssh-add -l
ssh-add ~/.ssh/id_rsa
ssh -T git@github.com
```

**Remote Not Accessible**
```bash
# Check network connectivity
ping github.com
# Try HTTPS instead of SSH
./scripts/core/init-repo-workflow.sh --repo-url https://github.com/myuser/myproject.git
```

**Submodule Path Conflict**
```bash
# Use different path or remove existing directory
rm -rf tools/formatter
# OR remove existing submodule
git submodule deinit tools/formatter
git rm tools/formatter
```

**Not in Git Repository**
```bash
# Initialize Git repository first
git init
# OR use --dir option
./scripts/core/create-orphan-branch.sh --branch dev/tools --dir /path/to/repo
```

See [Common Pitfalls Guide](./docs/guides/common-pitfalls.md) and [Workflow Examples](./docs/examples/repo-initialization-workflow-examples.md) for more troubleshooting help.

## Contributing

See [Contributing Guide](./docs/development/contributing.md) for details.

## License

[Add your license here]

## Links

- [Documentation](./docs/README.md)
- [Changelog](./docs/status/changelog.md)
- [Contributing](./docs/development/contributing.md)

---

**Version**: See [VERSION](./VERSION) file
**Last Updated**: 2026-02-13
