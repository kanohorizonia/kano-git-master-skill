# Git Master Skill - Documentation

**Version**: 0.1.0-beta
**Status**: Beta Release
**Last Updated**: 2026-02-13

## 📚 Documentation Structure

This documentation is organized into the following categories:

### 🚀 Getting Started
- [Quick Start Guide](./guides/quick-start.md) - Get started quickly
- [New Features Overview](./guides/new-features.md) - What's new in v0.1.0-beta

> **Note**: This is a beta release. Features are functional but may have rough edges. Feedback welcome!

### 📖 User Guides

#### Feature Guides
- [CPA Commit Plan Workflow](./guides/cpa-commit-plan-workflow.md) - Full-auto and semi-auto AI commit pipeline
- [Ignore Plan Operator Workflow](./guides/ignore-plan-operator-workflow.md) - Ignore stage command sequence, troubleshooting, and evidence checklist
- [Ignore Datasource Sync Policy](./guides/ignore-datasource-sync-policy.md) - Upstream github/gitignore sync and pinning policy
- [Repository Initialization Workflow](./guides/repo-initialization-workflow.md) - Automated repository setup
- [Version Information](./guides/version-info.md) - Extract version information
- [Worktree Management](./guides/worktree.md) - Manage multiple working trees
- [Subtree Management](./guides/subtree.md) - Include external repositories
- [Submodule Management](./guides/submodule.md) - Enhanced submodule operations
- [Mono-repo Optimization](./guides/scalar.md) - Git Scalar for large repos

#### VCS Bridge Guides
- [Git-P4 Guide](./guides/git-p4.md) - Perforce integration
- [Git-SVN Guide](./guides/git-svn.md) - Subversion integration

#### Best Practices
- [Common Pitfalls](./guides/common-pitfalls.md) - Avoid common issues in automation

### 🔄 Comparison Guides
- [Submodule vs Subtree](./comparisons/submodule-vs-subtree.md) - Which to use?
- [Git-P4 vs Git-SVN](./comparisons/git-p4-vs-git-svn.md) - VCS bridge comparison
- [When to Use Scalar](./comparisons/when-to-use-scalar.md) - Decision guide

### 🔀 Migration Guides
- [Perforce to Git](./migrations/perforce-to-git.md) - Complete migration guide
- [SVN to Git](./migrations/svn-to-git.md) - Complete migration guide

### 🏗️ Architecture
- [Worktree & Scalar Design](./design/worktree-scalar.md) - Technical architecture
- [Orphan Branch Design](./design/orphan-branch.md) - Integration design
- [Workspace Native Planner Contract](./design/workspace-native-planner-contract.md) - JSON contract for native planner and shell adapters

### 🧩 API Reference
- [API Overview](./api/overview.md) - Entry page for generated C++ API docs

### 📊 Project Information
- [Changelog](./status/changelog.md) - Version history and release notes

### 🔧 Contributing
- [Contributing Guide](./development/contributing.md) - How to contribute
- [Kano C++ Dev Convention](./development/kano-cpp-dev-convention.md) - Repo-local C++ include and structure rules
- [Testing Guide](./development/testing.md) - Running tests
- [Pixi Development Environment](./development/pixi.md) - Repo-local tools and task entrypoints

## 📁 Directory Structure

```
docs/
├── README.md                    # This file
├── guides/                      # User guides
│   ├── quick-start.md
│   ├── new-features.md
│   ├── cpa-commit-plan-workflow.md
│   ├── ignore-plan-operator-workflow.md
│   ├── ignore-datasource-sync-policy.md
│   ├── repo-initialization-workflow.md
│   ├── version-info.md
│   ├── worktree.md
│   ├── subtree.md
│   ├── submodule.md
│   ├── scalar.md
│   ├── git-p4.md
│   └── git-svn.md
├── examples/                    # Usage examples
│   ├── repo-initialization-workflow-examples.md
│   ├── init-kano-dev-skill-example.md
│   └── root-repo-multi-remote-examples.md
├── comparisons/                 # Comparison guides
│   ├── submodule-vs-subtree.md
│   ├── git-p4-vs-git-svn.md
│   └── when-to-use-scalar.md
├── migrations/                  # Migration guides
│   ├── perforce-to-git.md
│   └── svn-to-git.md
├── design/                      # Architecture docs
│   ├── worktree-scalar.md
│   ├── orphan-branch.md
│   └── workspace-native-planner-contract.md
├── status/                      # Project information
│   └── changelog.md             # Version history
└── development/                 # Contributing docs
    ├── contributing.md
    ├── kano-cpp-dev-convention.md
    ├── pixi.md
    └── testing.md
```

## 🎯 Quick Links

### Most Popular
1. [Quick Start Guide](./guides/quick-start.md)
2. [Repository Initialization Workflow](./guides/repo-initialization-workflow.md)
3. [Worktree Management](./guides/worktree.md)
4. [Common Pitfalls](./guides/common-pitfalls.md)
5. [Submodule vs Subtree](./comparisons/submodule-vs-subtree.md)
6. [When to Use Scalar](./comparisons/when-to-use-scalar.md)
7. [API Overview](./api/overview.md)

### For Migrations
1. [Perforce to Git](./migrations/perforce-to-git.md)
2. [SVN to Git](./migrations/svn-to-git.md)

### For Large Repos
1. [Mono-repo Optimization](./guides/scalar.md)
2. [When to Use Scalar](./comparisons/when-to-use-scalar.md)

## 📝 Documentation Conventions

### File Naming
- Use lowercase with hyphens (kebab-case): `my-guide.md`
- Be descriptive: `perforce-to-git.md` not `p4-migration.md`
- Group by category in subdirectories

### Document Structure
- Start with overview/summary
- Include table of contents for long docs
- Use clear headings (H2, H3)
- Include code examples
- Add "See Also" section at end

### Code Examples
- Use bash for shell commands
- Include comments for clarity
- Show expected output when helpful
- Use `--dry-run` in examples when possible

## 🔍 Finding Documentation

### By Feature
- **Repository Initialization**: [guides/repo-initialization-workflow.md](./guides/repo-initialization-workflow.md)
- **AI Commit (CPA)**: [guides/cpa-commit-plan-workflow.md](./guides/cpa-commit-plan-workflow.md)
- **Ignore Stage Ops**: [guides/ignore-plan-operator-workflow.md](./guides/ignore-plan-operator-workflow.md)
- **Ignore Datasource Sync**: [guides/ignore-datasource-sync-policy.md](./guides/ignore-datasource-sync-policy.md)
- **Version Info**: [guides/version-info.md](./guides/version-info.md)
- **Worktrees**: [guides/worktree.md](./guides/worktree.md)
- **Subtrees**: [guides/subtree.md](./guides/subtree.md)
- **Submodules**: [guides/submodule.md](./guides/submodule.md)
- **Scalar**: [guides/scalar.md](./guides/scalar.md)
- **Git-P4**: [guides/git-p4.md](./guides/git-p4.md)
- **Git-SVN**: [guides/git-svn.md](./guides/git-svn.md)
- **Common Pitfalls**: [guides/common-pitfalls.md](./guides/common-pitfalls.md)

### By Task
- **Initialize Repository**: [guides/repo-initialization-workflow.md](./guides/repo-initialization-workflow.md)
- **Parallel Development**: [guides/worktree.md](./guides/worktree.md)
- **Vendor Dependencies**: [guides/subtree.md](./guides/subtree.md)
- **Optimize Large Repo**: [guides/scalar.md](./guides/scalar.md)
- **Migrate from Perforce**: [migrations/perforce-to-git.md](./migrations/perforce-to-git.md)
- **Migrate from SVN**: [migrations/svn-to-git.md](./migrations/svn-to-git.md)

### By Question
- **Submodule or Subtree?**: [comparisons/submodule-vs-subtree.md](./comparisons/submodule-vs-subtree.md)
- **Should I use Scalar?**: [comparisons/when-to-use-scalar.md](./comparisons/when-to-use-scalar.md)
- **Git-P4 or Git-SVN?**: [comparisons/git-p4-vs-git-svn.md](./comparisons/git-p4-vs-git-svn.md)

## 📞 Getting Help

### Documentation Issues
- Found a typo? Please submit a PR
- Documentation unclear? Open an issue
- Missing information? Let us know

### Script Issues
- Check the script's `--help` first
- Try `--dry-run` to see what would happen
- Check the relevant guide in `docs/guides/`

### General Questions
- Check the comparison guides in `docs/comparisons/`
- Review the migration guides in `docs/migrations/`
- Look at the design docs in `docs/design/`

## 🔄 Documentation Updates

This documentation is actively maintained. Last major update: 2026-02-13

### Current Status
- **Version**: 0.1.0-beta
- **Status**: Beta release - all core features implemented and functional
- **Feedback**: Welcome! Please report issues or suggestions

### Recent Changes
- Reorganized into clear directory structure
- Renamed files to lowercase kebab-case
- Added comprehensive README (this file)
- Created category-based organization
- Removed internal development documents

### Contributing
See [development/contributing.md](./development/contributing.md) for how to contribute to documentation.

---

**Note**: This is the main documentation index. For script-specific help, use `script-name.sh --help`.
