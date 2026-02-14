# Git Master Skill

**Version**: 0.1.0-beta  
**Status**: Beta Release

Advanced Git automation scripts for power users and teams.

## Quick Start

```bash
# Show skill version
./scripts/internal/show-version.sh

# Get started
cat docs/README.md
```

## Features

- **Version Information** - Extract version info from git, git-p4, git-svn
- **Worktree Management** - Manage multiple working trees efficiently
- **Subtree Management** - Include external repositories as subtrees
- **Submodule Enhancement** - Enhanced submodule operations
- **Mono-repo Optimization** - Git Scalar for large repositories (10-20x faster)
- **VCS Bridges** - Integrate with Perforce (git-p4) and Subversion (git-svn)

## Documentation

All documentation is in the `docs/` directory:

- [Documentation Index](./docs/README.md) - Start here
- [Quick Start Guide](./docs/guides/quick-start.md)
- [Changelog](./docs/status/changelog.md)

## Installation

No installation required. Clone and use:

```bash
git clone <repo-url>
cd kano-git-master-skill

# Run any script
./scripts/worktree/create-worktree.sh --help
```

## Requirements

- Git 2.x or higher
- Bash 4.x or higher (or Git Bash on Windows)
- Python 3.x (for git-p4 features)
- Git Scalar (optional, for mono-repo optimization)

## Project Structure

```
kano-git-master-skill/
├── VERSION                 # Version number (single source of truth)
├── SKILL.md                # Skill documentation
├── docs/                   # User documentation
│   ├── README.md           # Documentation index
│   ├── guides/             # User guides
│   ├── comparisons/        # Feature comparisons
│   ├── migrations/         # Migration guides
│   └── status/             # Changelog
└── scripts/                # All automation scripts
    ├── internal/           # Skill maintenance scripts
    │   ├── show-version.sh # Show skill version
    │   ├── bump-version.sh # Bump skill version
    │   └── create-tag.sh   # Create git tag
    ├── tags/               # Tag management
    │   └── list-tags.sh    # List git tags
    ├── core/               # Core operations
    ├── worktree/           # Worktree management
    ├── subtree/            # Subtree management
    ├── submodules/         # Submodule operations
    ├── mono-repo/          # Mono-repo optimization
    ├── vcs-bridges/        # VCS integration (P4, SVN)
    └── lib/                # Helper libraries
```

## Usage Examples

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
