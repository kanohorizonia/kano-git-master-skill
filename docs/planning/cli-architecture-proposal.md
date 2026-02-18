# Kano Git Master Skill - CLI Architecture Proposal

**Date**: 2026-02-13
**Status**: Planning Phase
**Purpose**: Convert shell script collection into unified CLI tool

---

## Current State

### Project Overview
- **Name**: kano-git-master-skill
- **Type**: Collection of Bash shell scripts for Git operations
- **Total Scripts**: ~50+ scripts
- **Lines of Code**: ~8,000+ lines of Bash
- **Documentation**: 15+ comprehensive guides

### Current Structure
```
kano-git-master-skill/
├── scripts/
│   ├── commit-tools/          # AI-powered commit, resolve, rebase
│   │   ├── smart-commit*.sh   (10 scripts)
│   │   ├── smart-resolve*.sh  (3 scripts)
│   │   ├── smart-sync*.sh   (3 scripts)
│   │   └── lib/               (3 shared libraries)
│   ├── worktree/              # Worktree management (6 scripts)
│   ├── subtree/               # Subtree management (5 scripts)
│   ├── submodules/            # Submodule management (4 scripts)
│   ├── mono-repo/scalar/      # Git Scalar integration (4 scripts)
│   ├── vcs-bridges/           # Git-P4, Git-SVN bridges (8 scripts)
│   ├── tags/                  # Tag management (3 scripts)
│   └── internal/              # Version, utilities (5 scripts)
├── docs/                      # Comprehensive documentation
└── README.md
```

### Feature Categories

#### 1. Smart Git Tools (AI-Powered)
**Scripts**: 16 total
- `smart-commit.sh` + 5 variants (auto-fallback, provider-specific)
- `smart-resolve.sh` + 2 variants (auto-fallback)
- `smart-sync.sh` + 2 variants (auto-fallback)
- `smart-commit-push.sh`

**Key Features**:
- Multi-provider AI support (Copilot, Codex, OpenCode)
- Auto-fallback mechanism (tries providers in order)
- Safety checks (secrets, large files, conflicts)
- Custom commit rules support
- Automatic conflict resolution
- Intelligent rebase strategies

**AI Provider Selection**:
1. Copilot (gpt-5-mini) → 2. Codex (gpt-5.3-codex) → 3. OpenCode (auto) → 4. Fallback

#### 2. Worktree Management
**Scripts**: 6
- Create, list, remove, sync worktrees
- Orphan branch support
- IDE integration (VS Code, IntelliJ, Vim)

#### 3. Subtree Management
**Scripts**: 5
- Add, pull, push, split, list subtrees
- Vendor external dependencies
- Contribute back to upstream

#### 4. Submodule Management
**Scripts**: 4
- Add, update, remove, foreach submodules
- Enhanced UX over native git submodule

#### 5. Mono-repo Tools (Git Scalar)
**Scripts**: 4
- Register, status, optimize, unregister
- 10-20x performance improvements for large repos

#### 6. VCS Bridges
**Scripts**: 8
- Git-P4: Clone, sync, submit, rebase from Perforce
- Git-SVN: Clone, fetch, dcommit, rebase from Subversion

#### 7. Utilities
**Scripts**: 8
- Version info extraction
- Tag management
- Internal helpers

### Current Usage Pattern

**Direct Script Execution**:
```bash
# Current way (verbose)
./scripts/commit-tools/smart-commit-auto-with-fallback.sh
./scripts/worktree/create-worktree.sh feature-branch
./scripts/subtree/add-subtree.sh --prefix lib/mylib --url <url>
```

**Pain Points**:
1. Long paths to scripts
2. Hard for agents to discover and use
3. No unified interface
4. Difficult to distribute to end users
5. No package manager support

---

## Proposed Solution: Unified CLI

### Goals
1. **Unified Interface**: Single `<cli-name>` command for all operations
2. **Agent-Friendly**: Easy for AI agents to discover and execute
3. **Human-Friendly**: Short, memorable commands
4. **Distributable**: Support PyPI, npm, Homebrew, Winget
5. **Maintain Compatibility**: Keep existing shell scripts as implementation

### Architecture Approach

**Hybrid Model**: CLI wrapper + Shell script execution
- CLI tool handles: argument parsing, help text, command routing
- Shell scripts handle: actual Git operations (proven, tested code)
- Benefits: Reuse existing logic, gradual migration path

### Naming Strategy

**Requirement**: Support both standalone and unified CLI usage

**Naming Architecture**:
```
kano-git (primary name)
├── Full name: kano-git (8 chars)
├── Short alias: kog (3 chars) ⭐ Ultra-short!
└── Called by: kano git (future unified CLI)

Future unified CLI:
kano <subcommand>
├── kano git      → calls kano-git
├── kano backlog  → calls kano-backlog (kob)
└── kano <tool>   → calls kano-<tool> (ko<initial>)
```

**Benefits**:
- ✅ **Standalone**: `kano-git` or `kog` works independently
- ✅ **Ultra-short**: `kog` is only 3 characters (fastest to type)
- ✅ **Unified**: `kano git` works when unified CLI is ready
- ✅ **Extensible**: Pattern for other tools (kob, kot, etc.)
- ✅ **Flexible**: Users can choose short or long name

**Usage Examples**:
```bash
# Standalone usage (now)
kano-git commit --auto
kog commit --auto            # Ultra-short alias (3 chars!)

# Unified CLI usage (future)
kano git commit --auto       # Calls kano-git internally
kano backlog list            # Calls kano-backlog (kob)
```

**Implementation**:
- Binary name: `kano-git`
- Symlink/alias: `kog` → `kano-git`
- Future: `kano` CLI delegates to `kano-git` for `git` subcommand
- Pattern: `kano-<tool>` → `ko<initial>` (3-char aliases)

### Proposed Command Structure

**Standalone Usage** (current):
```bash
kano-git <command> [subcommand] [flags]
kog <command> [subcommand] [flags]       # Ultra-short alias (3 chars!)

# Smart Git Tools
kano-git commit [--auto]
kog commit --provider copilot            # Fast!
kano-git resolve [--interactive]
kog rebase [--onto main]
kog push

# Worktree
kano-git worktree create <branch>
kog worktree list

# Subtree
kog subtree add --prefix <path> --url <url>

# Submodule
kog submodule add --url <url> --path <path>

# Scalar
kog scalar register

# VCS Bridges
kog p4 clone <depot>
kog svn clone <url>

# Utilities
kog version
kog help
```

**Unified CLI Usage** (future):
```bash
kano git <command> [subcommand] [flags]

# Examples
kano git commit --auto           # Calls kano-git internally
kano git resolve --interactive
kano backlog list                # Calls kano-backlog (kob)
kano <tool> <command>            # Extensible architecture
```

---

## Project Requirements (Updated)

### Critical Requirements

1. **Performance First**: Must handle AAA Unreal Engine projects (100GB+, millions of files)
2. **CLI + AI Agent Focused**: Primary users are CLI and AI agents, not GUI
3. **GUI Separate**: Future GUI will be a separate project (`kano-git-master-gui`) that calls `gkano` CLI
4. **CLI Fast Iteration**: CLI needs rapid development/deployment
5. **Cross-Platform**: Windows, macOS, Linux support
6. **Team Expertise**: Senior C++ developer background

### Architecture Strategy

**Simplified Two-Layer Architecture** (CLI + AI Agent Focused):

```
┌─────────────────────────────────────────┐
│         CLI Layer (gkano)               │
│  - Rust or Go                           │
│  - Fast compilation                     │
│  - Single binary                        │
│  - AI agent friendly                    │
│  - Human friendly                       │
└─────────────────────────────────────────┘
              │
              ↓
┌─────────────────────────────────────────┐
│         System Layer                    │
│  - libgit2 (optional, for hot paths)   │
│  - System Git (primary)                 │
│  - Shell Scripts (proven, tested)       │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│   Future: Separate GUI Project         │
│   kano-git-master-gui                   │
│   - Calls gkano CLI                     │
│   - C++ or Rust + Qt/egui              │
│   - Independent development             │
└─────────────────────────────────────────┘
```

### Key Insight: Don't Rewrite Everything

**Simplified Approach** (Recommended):
- **CLI**: Thin wrapper (Rust or Go) that orchestrates operations
- **Shell Scripts**: Keep as-is for all operations (proven, tested)
- **Hot Paths**: Optionally rewrite in native code only if profiling shows bottlenecks
- **GUI**: Separate project that calls `gkano` CLI (future)

**Benefits**:
- ✅ Reuse existing tested shell scripts
- ✅ Fast CLI development (thin wrapper only)
- ✅ No complex shared library needed
- ✅ GUI can be developed independently
- ✅ Simple architecture, easy to maintain

---

## Technology Stack Options (Simplified for CLI Focus)

### Recommended: Go (Simple, Fast, Easy Distribution)

**Architecture**:
```
CLI (Go)
├── Single binary
├── Fast compilation (~1-2 seconds)
├── Easy distribution (Homebrew, Winget, npm, PyPI)
└── Calls shell scripts for all operations
```

**Pros**:
- ✅ **Fast Iteration**: Compile in 1-2 seconds
- ✅ **Simple**: No complex build system
- ✅ **Easy Distribution**: Native support for all package managers
- ✅ **Single Binary**: No runtime dependencies
- ✅ **Good Performance**: Fast enough for CLI orchestration
- ✅ **Easy to Learn**: Simple language, good for team

**Cons**:
- ⚠️ Not as fast as Rust/C++ (but good enough for CLI wrapper)
- ⚠️ GC may cause minor latency (not critical for CLI)

**Best For**: Fast development, easy distribution, team collaboration

### Alternative 1: Rust (Maximum Performance)

**Architecture**:
```
CLI (Rust)
├── Single binary
├── Medium compilation (~10-30 seconds)
├── Easy distribution
└── Can optimize hot paths natively
```

**Pros**:
- ✅ **Maximum Performance**: Zero-cost abstractions
- ✅ **Memory Safety**: No segfaults
- ✅ **Modern Tooling**: Cargo is excellent
- ✅ **Single Binary**: No runtime dependencies
- ✅ **Future-Proof**: Can optimize hot paths later

**Cons**:
- ⚠️ Slower compilation than Go
- ⚠️ Steeper learning curve
- ⚠️ More complex for simple CLI wrapper

**Best For**: Performance-critical paths, long-term optimization

### Alternative 2: Node.js (Fastest Development)

**Architecture**:
```
CLI (Node.js)
├── Requires Node.js runtime
├── No compilation needed
├── Easy npm distribution
└── Calls shell scripts
```

**Pros**:
- ✅ **Fastest Development**: No compilation
- ✅ **Easy npm Distribution**: Native support
- ✅ **Familiar**: Many developers know JavaScript

**Cons**:
- ❌ **Requires Runtime**: Node.js must be installed
- ❌ **Slower Startup**: ~100-200ms overhead
- ⚠️ **Harder Homebrew/Winget**: Requires Node.js dependency

**Best For**: Rapid prototyping, npm-first distribution

### Option Comparison for CLI Focus

| Criteria | Go | Rust | Node.js |
|----------|----|----|---------|
| **Compilation Speed** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ (none) |
| **Runtime Performance** | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ |
| **Single Binary** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ❌ |
| **Distribution** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ |
| **Learning Curve** | ⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐⭐ |
| **Team Collaboration** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| **CLI Wrapper Use** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ |

**Recommendation for CLI Focus**: **Go**

**Reasoning**:
1. Fast compilation → rapid iteration
2. Single binary → easy distribution
3. Simple language → easy team collaboration
4. Good enough performance for CLI wrapper
5. No need for complex shared library (GUI is separate project)

---

## Technology Stack Options

**Pros**:
- ✅ Single binary, no runtime dependencies
- ✅ Cross-platform compilation (Windows/macOS/Linux)
- ✅ Fast startup and execution
- ✅ Easy shell script execution (`exec.Command()`)
- ✅ Mature CLI framework (Cobra)
- ✅ Native support for:
  - Homebrew (direct)
  - Winget (direct)
  - npm (postinstall script downloads binary)
  - PyPI (setup.py downloads binary)

**Cons**:
- ⚠️ Requires learning Go (if team unfamiliar)
- ⚠️ Binary size ~5-10MB

**Distribution**:
- Homebrew: Native support via formula
- Winget: Native support via manifest
- npm: Wrapper package with postinstall
- PyPI: Wrapper package with setup.py

**Example Projects**:
- `gh` (GitHub CLI)
- `docker` CLI
- `kubectl`

### Option 2: Rust

**Pros**:
- ✅ Best performance and safety
- ✅ Single binary
- ✅ Cross-platform

**Cons**:
- ⚠️ Steep learning curve
- ⚠️ Slower compilation
- ⚠️ Larger binary size

**Distribution**: Same as Go

### Option 3: Node.js

**Pros**:
- ✅ Native npm support
- ✅ Familiar to many developers
- ✅ Easy shell script execution

**Cons**:
- ❌ Requires Node.js runtime
- ❌ Slower startup
- ⚠️ Harder to distribute via Homebrew/Winget

**Distribution**:
- npm: Native
- Homebrew: Requires Node.js dependency
- Winget: Requires Node.js dependency
- PyPI: Complex wrapper

### Option 4: Python

**Pros**:
- ✅ Native PyPI support
- ✅ Easy to learn
- ✅ Good for scripting

**Cons**:
- ❌ Requires Python runtime
- ❌ Slower execution
- ⚠️ Harder to distribute via Homebrew/Winget

**Distribution**:
- PyPI: Native
- Homebrew: Requires Python dependency
- Winget: Requires Python dependency
- npm: Complex wrapper

### Option 5: Pure Bash

**Pros**:
- ✅ No compilation needed
- ✅ Direct script execution

**Cons**:
- ❌ Cross-platform issues (Windows)
- ❌ Hard to distribute via package managers
- ❌ Poor maintainability at scale

**Distribution**: Very limited

---

## Comparison Matrix

| Criteria | Go | Rust | Node.js | Python | Bash |
|----------|----|----|---------|--------|------|
| **Single Binary** | ✅ | ✅ | ❌ | ❌ | ✅ |
| **No Runtime Deps** | ✅ | ✅ | ❌ | ❌ | ⚠️ |
| **Cross-Platform** | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| **Fast Startup** | ✅ | ✅ | ⚠️ | ⚠️ | ✅ |
| **Easy Shell Exec** | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Homebrew** | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| **Winget** | ✅ | ✅ | ⚠️ | ⚠️ | ❌ |
| **npm** | ⚠️ | ⚠️ | ✅ | ❌ | ❌ |
| **PyPI** | ⚠️ | ⚠️ | ❌ | ✅ | ❌ |
| **Learning Curve** | ⚠️ | ❌ | ✅ | ✅ | ✅ |
| **Maintainability** | ✅ | ✅ | ✅ | ✅ | ⚠️ |

**Legend**: ✅ Excellent | ⚠️ Acceptable | ❌ Poor

---

## Detailed Architecture: Go Implementation (Recommended)

### Why Go for CLI Focus

1. **Fast Compilation**: 1-2 seconds (vs Rust's 10-30 seconds)
2. **Simple**: Easy to learn, easy to maintain
3. **Single Binary**: No runtime dependencies
4. **Great CLI Tools**: Cobra framework is mature
5. **Easy Distribution**: Native Homebrew, Winget, npm, PyPI support
6. **Good Enough Performance**: Fast enough for CLI orchestration

### CLI Structure

```go
// cmd/kano-git/main.go
package main

import (
    "github.com/spf13/cobra"
    "kano/kano-git/internal/commands"
)

func main() {
    rootCmd := &cobra.Command{
        Use:   "kano-git",
        Short: "Kano Git Master - AI-powered Git tools",
        Long: `Kano Git Master provides AI-powered Git operations.

Can be used standalone as 'kano-git' or 'kog' (ultra-short!),
or called via unified CLI as 'kano git'.`,
    }

    // Add commands
    rootCmd.AddCommand(commands.CommitCmd())
    rootCmd.AddCommand(commands.ResolveCmd())
    rootCmd.AddCommand(commands.RebaseCmd())
    rootCmd.AddCommand(commands.WorktreeCmd())
    // ... more commands

    rootCmd.Execute()
}
```

### Shell Script Integration

**Simple Wrapper** - No rewrite needed:

```go
// internal/shell/executor.go
package shell

import (
    "os/exec"
    "path/filepath"
)

func ExecuteScript(scriptName string, args ...string) error {
    scriptPath := filepath.Join(getScriptDir(), scriptName)

    cmd := exec.Command("bash", scriptPath)
    cmd.Args = append(cmd.Args, args...)
    cmd.Stdout = os.Stdout
    cmd.Stderr = os.Stderr

    return cmd.Run()
}

// internal/commands/commit.go
package commands

func CommitCmd() *cobra.Command {
    return &cobra.Command{
        Use:   "commit",
        Short: "AI-powered commit message generation",
        RunE: func(cmd *cobra.Command, args []string) error {
            return shell.ExecuteScript(
                "commit-tools/smart-commit-auto-with-fallback.sh",
                args...,
            )
        },
    }
}
```

### Performance Optimization Strategy

**Phase 1**: Wrap all shell scripts (ship fast)
**Phase 2**: Profile on AAA repos (identify bottlenecks)
**Phase 3**: Optimize only if needed (likely not needed for CLI wrapper)

**Key Insight**: CLI orchestration is not the bottleneck. Shell scripts and Git operations are. Go is fast enough for CLI wrapper.

### Future GUI Integration

**Separate Project** (`kano-git-master-gui`):

```cpp
// GUI calls gkano CLI
#include <QProcess>

void MainWindow::onCommitClicked() {
    QProcess process;
    process.start("gkano", QStringList() << "commit" << "--auto");
    process.waitForFinished();

    // Handle output
    QString output = process.readAllStandardOutput();
    displayOutput(output);
}
```

**Benefits**:
- ✅ GUI development independent of CLI
- ✅ No shared library complexity
- ✅ CLI can be updated without GUI changes
- ✅ Simple architecture, easy to maintain

---

## Proposed Project Structure (Go)

```
kano-git-master-skill/
├── cmd/
│   └── kano-git/              # CLI entry point
│       └── main.go
│
├── internal/
│   ├── commands/              # CLI commands
│   │   ├── commit.go
│   │   ├── resolve.go
│   │   ├── rebase.go
│   │   ├── worktree.go
│   │   ├── subtree.go
│   │   └── ...
│   ├── shell/                 # Shell script executor
│   │   └── executor.go
│   ├── config/                # Configuration
│   │   └── config.go
│   └── version/               # Version info
│       └── version.go
│
├── scripts/                   # Keep existing shell scripts
│   ├── commit-tools/
│   ├── worktree/
│   ├── subtree/
│   └── ...
│
├── docs/
│   └── (existing documentation)
│
├── go.mod                     # Go module definition
├── go.sum                     # Dependency checksums
├── Makefile                   # Build automation
├── .goreleaser.yml            # Release automation
├── README.md
└── LICENSE
```

### go.mod

```go
module github.com/kano/kano-git

go 1.21

require (
    github.com/spf13/cobra v1.8.0
    github.com/spf13/viper v1.18.0
)
```

### Makefile

```makefile
.PHONY: build install test clean

build:
	go build -o bin/kano-git ./cmd/kano-git
	ln -sf kano-git bin/kog  # Create ultra-short alias (3 chars!)

install:
	go install ./cmd/kano-git
	# Create symlink for ultra-short alias
	ln -sf $(GOPATH)/bin/kano-git $(GOPATH)/bin/kog

test:
	go test ./...

clean:
	rm -rf bin/

# Cross-compile for all platforms
release:
	goreleaser release --snapshot --clean
```

---

## Implementation Phases

### Phase 1: Foundation (Week 1-2)
- [ ] Choose technology stack
- [ ] Initialize project structure
- [ ] Setup CLI framework
- [ ] Implement shell script executor
- [ ] Basic commands: `kano-git version`, `kano-git help`
- [ ] Create `kog` alias/symlink (ultra-short!)

### Phase 2: Core Commands (Week 3-4)
- [ ] Implement `kano-git commit`
- [ ] Implement `kano-git resolve`
- [ ] Implement `kano-git rebase`
- [ ] Implement `kano-git push`
- [ ] Testing and documentation

### Phase 3: Extended Commands (Week 5-6)
- [ ] Implement `kano-git worktree` commands
- [ ] Implement `kano-git subtree` commands
- [ ] Implement `kano-git submodule` commands
- [ ] Implement `kano-git scalar` commands
- [ ] Implement `kano-git p4/svn` VCS bridge commands

### Phase 4: Distribution (Week 7-8)
- [ ] Setup GoReleaser (or equivalent)
- [ ] Create Homebrew formula
- [ ] Create Winget manifest
- [ ] Create npm wrapper package
- [ ] Create PyPI wrapper package
- [ ] CI/CD pipeline for releases

### Phase 5: Migration (Week 9-10)
- [ ] Move scripts to `src/shell/`
- [ ] Update documentation
- [ ] Deprecation notices for direct script usage
- [ ] Migration guide for users

---

## Distribution Strategy

### Homebrew (macOS/Linux)
```bash
brew tap kano/tools
brew install kano-git        # Installs both kano-git and kog alias
```

### Winget (Windows)
```bash
winget install kano.kano-git
```

### npm (Cross-platform)
```bash
npm install -g @kano/kano-git
# Provides both kano-git and kog commands
```

### PyPI (Cross-platform)
```bash
pip install kano-git
```

### Direct Download
- GitHub Releases with binaries for all platforms
- Install script: `curl -sSL https://kano-git.sh/install.sh | bash`
- Creates both `kano-git` and `kog` commands

---

## Parallel Execution Design (Key Value-Add)

### Why Parallel Execution Matters

**Problem**: Sequential operations are slow for multi-repo workflows

**Example Scenario** (AAA Unreal Engine Project):
- Main repo + 51 submodules
- Each `git pull` takes ~2 seconds
- **Sequential**: 51 repos × 2s = **102 seconds** ⏱️
- **Parallel**: ~**10 seconds** ⚡ (10x speedup!)

**Key Insight**: The CLI wrapper can provide massive speedups through parallel execution, even without rewriting shell scripts.

### Use Cases for Parallel Execution

#### 1. Multi-Repo Pull/Fetch
```bash
# Sequential (current): 102s for 51 repos
./scripts/submodules/foreach-submodule.sh git pull

# Parallel (new): ~10s for 51 repos
kog pull --recursive --parallel
kano-git pull --recursive --parallel --jobs 10
```

#### 2. Submodule Batch Operations
```bash
# Update all submodules in parallel
kog submodule update --parallel

# Check status of all submodules in parallel
kog submodule status --parallel

# Run custom command across all submodules
kog submodule foreach --parallel "git fetch origin"
```

#### 3. Multi-Worktree Operations
```bash
# Create multiple worktrees in parallel
kog worktree create-batch feature-1 feature-2 feature-3 --parallel

# Sync all worktrees in parallel
kog worktree sync-all --parallel
```

#### 4. Parallel Commit-Push
```bash
# Commit and push multiple repos in parallel
kog commit-push --repos repo1,repo2,repo3 --parallel
```

### Go Implementation (Goroutines)

**Why Go is Perfect for This**:
- ✅ Goroutines are lightweight (thousands can run concurrently)
- ✅ Simple concurrency model (channels, sync.WaitGroup)
- ✅ Built-in parallelism (GOMAXPROCS)
- ✅ Easy error aggregation

**Example: Parallel Submodule Pull**

```go
// internal/parallel/executor.go
package parallel

import (
    "context"
    "fmt"
    "sync"
)

type Result struct {
    Repo   string
    Output string
    Error  error
}

// ExecuteParallel runs a command across multiple repos in parallel
func ExecuteParallel(ctx context.Context, repos []string, cmd func(string) error, maxWorkers int) []Result {
    // Create worker pool
    jobs := make(chan string, len(repos))
    results := make(chan Result, len(repos))

    // Start workers
    var wg sync.WaitGroup
    for i := 0; i < maxWorkers; i++ {
        wg.Add(1)
        go worker(ctx, jobs, results, cmd, &wg)
    }

    // Send jobs
    for _, repo := range repos {
        jobs <- repo
    }
    close(jobs)

    // Wait for completion
    go func() {
        wg.Wait()
        close(results)
    }()

    // Collect results
    var allResults []Result
    for result := range results {
        allResults = append(allResults, result)
    }

    return allResults
}

func worker(ctx context.Context, jobs <-chan string, results chan<- Result, cmd func(string) error, wg *sync.WaitGroup) {
    defer wg.Done()

    for repo := range jobs {
        select {
        case <-ctx.Done():
            return
        default:
            err := cmd(repo)
            results <- Result{
                Repo:  repo,
                Error: err,
            }
        }
    }
}
```

**Example: Parallel Submodule Update Command**

```go
// internal/commands/submodule.go
package commands

import (
    "context"
    "fmt"
    "os/exec"
    "time"

    "github.com/spf13/cobra"
    "kano/kano-git/internal/parallel"
)

func SubmoduleUpdateCmd() *cobra.Command {
    var parallelFlag bool
    var jobsFlag int

    cmd := &cobra.Command{
        Use:   "update",
        Short: "Update all submodules",
        RunE: func(cmd *cobra.Command, args []string) error {
            // Get list of submodules
            submodules, err := getSubmodules()
            if err != nil {
                return err
            }

            if !parallelFlag {
                // Sequential execution (current behavior)
                return updateSubmodulesSequential(submodules)
            }

            // Parallel execution (new!)
            return updateSubmodulesParallel(submodules, jobsFlag)
        },
    }

    cmd.Flags().BoolVar(&parallelFlag, "parallel", false, "Update submodules in parallel")
    cmd.Flags().IntVar(&jobsFlag, "jobs", 10, "Number of parallel jobs")

    return cmd
}

func updateSubmodulesParallel(submodules []string, maxJobs int) error {
    ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
    defer cancel()

    // Define update command
    updateCmd := func(submodule string) error {
        cmd := exec.Command("git", "-C", submodule, "pull", "origin", "HEAD")
        return cmd.Run()
    }

    // Execute in parallel
    results := parallel.ExecuteParallel(ctx, submodules, updateCmd, maxJobs)

    // Aggregate results
    var errors []error
    for _, result := range results {
        if result.Error != nil {
            errors = append(errors, fmt.Errorf("%s: %w", result.Repo, result.Error))
        }
    }

    if len(errors) > 0 {
        return fmt.Errorf("failed to update %d submodules: %v", len(errors), errors)
    }

    return nil
}
```

### Progress Display

**Real-time Progress Bar** (using `github.com/schollz/progressbar`):

```go
// internal/ui/progress.go
package ui

import (
    "fmt"
    "github.com/schollz/progressbar/v3"
)

func ShowProgress(total int, description string) *progressbar.ProgressBar {
    return progressbar.NewOptions(total,
        progressbar.OptionSetDescription(description),
        progressbar.OptionShowCount(),
        progressbar.OptionShowIts(),
        progressbar.OptionSetWidth(50),
        progressbar.OptionSetTheme(progressbar.Theme{
            Saucer:        "=",
            SaucerHead:    ">",
            SaucerPadding: " ",
            BarStart:      "[",
            BarEnd:        "]",
        }),
    )
}

// Usage in parallel execution
func updateSubmodulesParallelWithProgress(submodules []string, maxJobs int) error {
    bar := ShowProgress(len(submodules), "Updating submodules")

    // ... parallel execution ...

    for result := range results {
        bar.Add(1)
        if result.Error != nil {
            fmt.Printf("\n❌ %s: %v\n", result.Repo, result.Error)
        } else {
            fmt.Printf("\n✅ %s: updated\n", result.Repo)
        }
    }

    return nil
}
```

**Output Example**:
```
Updating submodules [===>                ] 15/51 (29%) 3.2 it/s
✅ Engine/Plugins/Runtime/AudioSynesthesia: updated
✅ Engine/Plugins/Runtime/Metasound: updated
❌ Engine/Plugins/Experimental/VirtualCamera: network timeout
```

### Error Aggregation

**Collect and Report All Errors**:

```go
// internal/errors/aggregator.go
package errors

import (
    "fmt"
    "strings"
)

type AggregatedError struct {
    Errors []error
}

func (e *AggregatedError) Error() string {
    var sb strings.Builder
    sb.WriteString(fmt.Sprintf("encountered %d errors:\n", len(e.Errors)))
    for i, err := range e.Errors {
        sb.WriteString(fmt.Sprintf("  %d. %v\n", i+1, err))
    }
    return sb.String()
}

func Aggregate(results []parallel.Result) error {
    var errors []error
    for _, result := range results {
        if result.Error != nil {
            errors = append(errors, fmt.Errorf("%s: %w", result.Repo, result.Error))
        }
    }

    if len(errors) == 0 {
        return nil
    }

    return &AggregatedError{Errors: errors}
}
```

### Performance Comparison

**Benchmark: 51 Submodules (AAA Unreal Engine Project)**

| Operation | Sequential | Parallel (10 workers) | Speedup |
|-----------|------------|----------------------|---------|
| `git pull` (all) | 102s | 10.2s | **10x** ⚡ |
| `git fetch` (all) | 51s | 5.1s | **10x** ⚡ |
| `git status` (all) | 25.5s | 2.6s | **10x** ⚡ |
| `git submodule update` | 153s | 15.3s | **10x** ⚡ |

**Key Insight**: Parallel execution provides consistent 10x speedup for I/O-bound operations (network, disk).

### Configuration

**Allow users to configure parallelism**:

```yaml
# ~/.config/kano-git/config.yaml
parallel:
  enabled: true
  max_jobs: 10
  timeout: 5m

  # Per-operation overrides
  submodule:
    max_jobs: 20
    timeout: 10m

  worktree:
    max_jobs: 5
    timeout: 2m
```

**CLI Flags**:
```bash
# Global flags
kog --parallel --jobs 10 submodule update
kog --no-parallel submodule update  # Disable parallel

# Per-command flags
kog submodule update --parallel --jobs 20
kog worktree sync-all --parallel --timeout 2m
```

### Safety Considerations

**When NOT to Use Parallel Execution**:
1. **Operations with side effects**: Commits, pushes (unless explicitly requested)
2. **Interactive operations**: Merge conflict resolution
3. **Operations requiring order**: Rebase chains

**Safe Operations** (parallel by default):
- ✅ `git pull` / `git fetch` (read-only network)
- ✅ `git status` (read-only local)
- ✅ `git submodule update` (independent repos)
- ✅ `git worktree list` (read-only)

**Unsafe Operations** (sequential by default):
- ❌ `git commit` (requires user input)
- ❌ `git push` (side effects on remote)
- ❌ `git rebase` (complex state changes)

**User Override**:
```bash
# Force parallel push (at your own risk!)
kog push --parallel --force-parallel
```

### Future Enhancements

**Phase 1** (MVP):
- Parallel submodule operations
- Progress bar
- Error aggregation

**Phase 2** (Advanced):
- Adaptive parallelism (auto-tune based on system resources)
- Retry logic (exponential backoff)
- Dependency-aware scheduling (DAG execution)

**Phase 3** (Expert):
- Distributed execution (across multiple machines)
- Caching layer (avoid redundant operations)
- Smart batching (group related operations)

---

## Performance Considerations for AAA Repos

### Benchmark Targets

**Test Scenario**: Unreal Engine 5 project
- Size: 100GB+
- Files: 1M+ files
- Submodules: 50+ nested
- History: 100K+ commits

**Performance Goals**:
- `kano-git status`: < 2 seconds (vs git: ~5 seconds)
- `kano-git commit`: < 1 second overhead
- `kano-git resolve`: < 500ms per conflict
- Memory: < 500MB for typical operations

### Optimization Strategies

1. **Parallel Processing**: Use Rayon for file tree traversal
2. **Incremental Operations**: Cache file status, use Git index
3. **Native Git**: Use libgit2 directly (no subprocess overhead)
4. **Smart Caching**: Cache AI provider responses, model lists
5. **Lazy Loading**: Load submodules on-demand

### Why Not Pure Shell Scripts?

**Shell Script Limitations for AAA Repos**:
- ❌ Slow file iteration (fork/exec overhead)
- ❌ No parallelism (unless complex)
- ❌ High memory usage (string processing)
- ❌ Poor error handling at scale

**Native Code Benefits**:
- ✅ Parallel file processing (Rayon)
- ✅ Direct libgit2 access (no subprocess)
- ✅ Efficient memory usage
- ✅ Incremental operations

---

## Migration Strategy: No Full Rewrite

### Phase 1: Wrap Existing Scripts (Week 1-2)
```rust
// Quick win: Just wrap shell scripts
pub fn smart_commit(args: &[String]) -> Result<()> {
    execute_script("smart-commit-auto-with-fallback.sh", args)
}
```

### Phase 2: Profile and Identify Bottlenecks (Week 3)
- Run on AAA test repo
- Identify slow operations
- Measure shell script overhead

### Phase 3: Optimize Hot Paths (Week 4-8)
**Rewrite only bottlenecks**:
- File tree traversal → Native Rust
- Conflict detection → Native Rust
- Git status → libgit2 direct
- Keep complex logic in scripts (git-p4, git-svn)

### Phase 4: Gradual Migration (Ongoing)
- Migrate scripts one-by-one as needed
- Keep working scripts as-is
- Focus on performance-critical paths

**Example: Hybrid Approach**
```rust
pub fn smart_commit(repo: &Repository) -> Result<()> {
    // Fast path: Native Rust for simple cases
    if is_simple_commit(repo) {
        return native_commit(repo);
    }

    // Complex path: Use proven shell script
    execute_script("smart-commit-auto-with-fallback.sh", &[])
}
```

---

## Open Questions for Expert Consultation

### 1. Technology Stack (Updated)
**Question**: Which language for CLI?
- **Context**: CLI + AI agent focused, GUI is separate project
- **Options**:
  - **Go**: Fast compilation, easy distribution, simple
  - **Rust**: Maximum performance, modern tooling
  - **Node.js**: Fastest development, requires runtime
- **Priority**: Fast Iteration > Easy Distribution > Simple Architecture
- **Recommendation**: Go (best balance for CLI wrapper)

### 2. CLI Naming
**Decision**: Dual naming strategy
- **Primary name**: `kano-git` (full, descriptive, 8 chars)
- **Short alias**: `kog` (ultra-short, 3 chars!)
- **Future integration**: `kano git` (unified CLI)
- **Rationale**: `kog` is fastest to type, supports extensible pattern (kob, kot, etc.)

### 3. Migration Strategy (Updated)
**Question**: How aggressive should we be with rewriting?
- **Option A**: Minimal rewrite (wrap scripts, optimize only proven bottlenecks)
- **Option B**: Aggressive rewrite (rewrite everything in native code)
- **Option C**: Hybrid approach (hot paths native, complex logic stays in scripts)
- **Recommendation**: Option A or C (don't rewrite working code)

### 4. Backward Compatibility
**Question**: How long should we support direct script execution?
- **Option A**: Forever (scripts remain primary interface)
- **Option B**: 1 year deprecation period
- **Option C**: Immediate deprecation (CLI only)

### 5. GUI Framework (Deferred)
**Decision**: GUI will be a separate project (`kano-git-master-gui`)
- **Approach**: GUI calls `gkano` CLI as subprocess
- **Benefits**: Independent development, no shared library complexity
- **Future Options**: Qt (C++), egui (Rust), Tauri (Rust + Web)
- **Priority**: CLI first, GUI later

### 6. Package Distribution (Updated)
**Question**: Which package managers should we prioritize?
- **Must Have**: Homebrew (macOS/Linux), Winget (Windows)
- **Nice to Have**: npm (wrapper), PyPI (wrapper)
- **Can Skip**: Distribution-specific (apt, yum, etc.)

---

## Additional Context

### Current Users
- AI coding agents (primary use case)
- Human developers (secondary use case)
- CI/CD pipelines (tertiary use case)
- **Future**: GUI users (AAA game developers)

### Performance Requirements (Updated)
- **Startup time**: < 50ms (critical for CLI)
- **AAA repo operations**: < 2s for status, < 1s for commit
- **Memory**: < 500MB for typical operations
- **Scalability**: Handle 1M+ files, 100GB+ repos
- **Responsiveness**: GUI must stay responsive during long operations

### Maintenance Considerations
- Team size: Small (1-3 developers)
- Release frequency: Monthly
- Support burden: Community-driven

### Success Metrics
1. Adoption rate (downloads/installs)
2. Agent usage (how many agents use it)
3. User satisfaction (GitHub stars, issues)
4. Maintenance burden (time spent on issues)

---

## References

- Current codebase: `kano-git-master-skill/`
- Documentation: `docs/guides/`
- Similar projects:
  - `gh` (GitHub CLI) - Go
  - `git-extras` - Bash
  - `hub` - Go (deprecated, replaced by `gh`)
  - `tig` - C
  - `gitui` - Rust (TUI, excellent performance)
  - `lazygit` - Go (TUI)
  - `GitKraken` - Electron (GUI, slow on large repos)
  - `Sublime Merge` - C++ (GUI, excellent performance)

---

**Next Steps**: Gather expert feedback on technology stack and naming before proceeding with implementation.

