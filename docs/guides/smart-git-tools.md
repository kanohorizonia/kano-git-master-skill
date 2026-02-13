# Smart Git Tools - AI-Powered Git Automation

Complete guide to AI-powered Git tools in the Git Master Skill.

## Overview

Smart Git Tools leverage AI to automate and enhance common Git operations:
- Intelligent commit message generation
- Automatic conflict resolution
- Smart rebase strategies
- Safety checks and validation
- Multi-repository support

## Available Tools

### Core Tools

#### 1. smart-commit.sh
AI-generated commit messages with safety checks.

**Features:**
- Multi-provider AI support (OpenCode, Codex, Copilot)
- Automatic trailing whitespace fixing
- Safety checks (secrets, large files, conflicts)
- Custom commit rules support
- Auto .gitignore updates
- Optional AI review gate
- Repository filtering

**Usage:**
```bash
# Basic usage
./smart-commit.sh --provider copilot --model gpt-4o

# With custom rules
./smart-commit.sh --provider copilot --model gpt-4o --rules-file .github/commit-rules.md

# Only specific repos
./smart-commit.sh --provider copilot --model gpt-4o --repos ".,submodules/lib"

# List available models
./smart-commit.sh --list-models copilot
```

**Wrapper Scripts:**
```bash
# Convenience wrappers with defaults
./smart-commit-opencode.sh
./smart-commit-codex.sh
./smart-commit-copilot.sh
```

#### 2. smart-commit-push.sh
Complete workflow: commit → fetch → rebase → push.

**Features:**
- Automated commit workflow
- Fetch latest changes
- Rebase onto remote
- Conflict detection
- Safe push with --force-with-lease
- Repository filtering
- Dry-run mode

**Usage:**
```bash
# Full workflow
./smart-commit-push.sh --provider copilot --model gpt-4o

# Only specific repos
./smart-commit-push.sh --provider copilot --model gpt-4o --repos "."

# Dry run
./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run
```

#### 3. smart-resolve.sh
AI-powered conflict resolution.

**Features:**
- Automatic conflict detection
- Extract conflict markers and context
- AI-generated resolutions
- Interactive review mode
- Automatic mode (trust AI)
- Backup files (.conflict-backup)
- Conflict statistics
- Auto-stage resolved files

**Usage:**
```bash
# Auto-resolve all conflicts
./smart-resolve.sh --provider copilot --model gpt-4o

# Interactive mode (review each)
./smart-resolve.sh --provider copilot --model gpt-4o --interactive

# Resolve specific file
./smart-resolve.sh --provider copilot --model gpt-4o --file src/main.ts

# Dry run
./smart-resolve.sh --provider copilot --model gpt-4o --dry-run
```

**Workflow:**
1. Detects conflicted files
2. Extracts conflict markers (<<<<<<, =======, >>>>>>>)
3. Builds AI prompt with context
4. Gets AI resolution
5. Applies or reviews resolution
6. Stages resolved files
7. Continues merge/rebase operation

#### 4. smart-rebase.sh
AI-assisted intelligent rebase.

**Features:**
- Commit history analysis
- AI rebase strategy suggestions
- Auto-squash fixup commits
- Interactive rebase support
- Automatic conflict resolution
- Strategy selection (merge, ours, theirs)
- Dry-run mode

**Usage:**
```bash
# Rebase onto upstream
./smart-rebase.sh --provider copilot --model gpt-4o

# Rebase onto specific branch
./smart-rebase.sh --provider copilot --model gpt-4o --onto main

# Interactive with AI suggestions
./smart-rebase.sh --provider copilot --model gpt-4o --interactive

# Auto-squash fixup commits
./smart-rebase.sh --provider copilot --model gpt-4o --auto-squash

# Dry run
./smart-rebase.sh --provider copilot --model gpt-4o --dry-run
```

**AI Features:**
- Suggests which commits to squash
- Identifies fixup/WIP commits
- Recommends rebase strategy
- Auto-resolves simple conflicts
- Generates improved commit messages

## Shared Libraries

### lib/ai-providers.sh
AI provider abstraction layer.

**Functions:**
- `detect_opencode()`, `detect_codex()`, `detect_copilot()`
- `fetch_*_models()` - Get available models
- `list_available_models()` - Unified model listing
- `clear_cache()` - Clear model cache
- `ai_generate_message()` - Generate text via AI
- `ai_review_changes()` - AI review

**Cache:**
- Location: `~/.cache/kano-git-master-skill/models/`
- TTL: 24 hours
- Per-provider caching

### lib/git-helpers.sh
Common Git utility functions.

**Branch Operations:**
- `get_current_branch()` - Get current branch name
- `get_upstream_branch()` - Get upstream branch
- `has_upstream()` - Check if upstream exists

**Repository State:**
- `is_clean_working_tree()` - Check for uncommitted changes
- `has_staged_changes()` - Check for staged changes
- `has_unstaged_changes()` - Check for unstaged changes
- `is_detached_head()` - Check for detached HEAD

**Conflict Detection:**
- `has_conflicts()` - Check for conflicts
- `get_conflicted_files()` - List conflicted files
- `count_conflicts()` - Count conflicts

**Merge/Rebase State:**
- `is_merge_in_progress()` - Check for ongoing merge
- `is_rebase_in_progress()` - Check for ongoing rebase
- `is_cherry_pick_in_progress()` - Check for ongoing cherry-pick
- `get_operation_in_progress()` - Get current operation

**Validation:**
- `is_git_repo()` - Check if directory is a git repo
- `validate_repo()` - Validate repository

### lib/conflict-parser.sh
Conflict parsing and resolution helpers.

**Conflict Detection:**
- `has_conflict_markers()` - Check file for conflict markers
- `count_conflict_markers()` - Count conflicts in file
- `extract_conflict_sections()` - Extract all conflicts
- `parse_conflict_markers()` - Parse OURS vs THEIRS

**Context Extraction:**
- `get_conflict_context()` - Get surrounding context
- `build_conflict_prompt()` - Build AI prompt

**Resolution:**
- `apply_resolution()` - Apply AI resolution
- `get_conflict_stats()` - Get conflict statistics

## Custom Commit Rules

### Auto-Detection
Place rules file in one of these locations:
- `.git/commit-rules.md` (repo-specific, not committed)
- `.github/commit-rules.md` (committed, shared with team)
- `COMMIT_RULES.md` (root of repo)
- `.commit-rules` (hidden file)

### Explicit File
```bash
./smart-commit.sh --provider copilot --model gpt-4o --rules-file path/to/rules.md
```

### Inline Rules
```bash
./smart-commit.sh --provider copilot --model gpt-4o --rules "Use emoji prefixes"
```

### Example Rules

**Conventional Commits with Emoji:**
```markdown
# Commit Message Rules

Use Conventional Commits with emoji prefixes:

- ✨ feat: New feature
- 🐛 fix: Bug fix
- 📝 docs: Documentation changes
- ♻️ refactor: Code refactoring
- ✅ test: Test changes

Examples:
- ✨ feat(auth): add OAuth2 support
- 🐛 fix(api): handle null response

Rules:
- Keep summary under 60 characters
- Use present tense
- No period at the end
```

See `docs/examples/commit-rules-example.md` for more examples.

## Repository Filtering

All tools support `--repos` flag to process only specific repositories:

```bash
# Only root repo
--repos "."

# Root and specific submodule
--repos ".,submodules/my-lib"

# Multiple submodules
--repos "submodules/lib1,submodules/lib2"

# Absolute paths
--repos "/path/to/repo1,/path/to/repo2"
```

## Safety Features

### Automatic Checks
- Secret detection (API keys, tokens, credentials)
- Large file detection (configurable threshold)
- Conflict marker detection
- Trailing whitespace auto-fix
- .gitignore auto-update

### AI Review Gate
Optional AI safety review before commit:
```bash
# Enable (default)
--ai-review

# Disable
--no-ai-review
```

### Backup Files
- Conflict resolutions create `.conflict-backup` files
- Original content preserved
- Easy rollback if needed

### Dry-Run Mode
All tools support `--dry-run` to preview operations:
```bash
./smart-commit.sh --provider copilot --model gpt-4o --dry-run
./smart-resolve.sh --provider copilot --model gpt-4o --dry-run
./smart-rebase.sh --provider copilot --model gpt-4o --dry-run
```

## Common Workflows

### Daily Development
```bash
# 1. Make changes
# 2. Commit with AI message
./smart-commit-copilot.sh

# 3. Push with rebase
./smart-commit-push.sh --provider copilot --model gpt-4o
```

### Conflict Resolution
```bash
# 1. Attempt merge/rebase
git merge feature-branch
# or
git rebase main

# 2. If conflicts occur
./smart-resolve.sh --provider copilot --model gpt-4o --interactive

# 3. Continue operation
git merge --continue
# or
git rebase --continue
```

### Clean Up History
```bash
# 1. Interactive rebase with AI suggestions
./smart-rebase.sh --provider copilot --model gpt-4o --interactive --auto-squash

# 2. Force push safely
git push --force-with-lease
```

### Multi-Repository Workflow
```bash
# Only commit root and specific submodule
./smart-commit.sh --provider copilot --model gpt-4o --repos ".,submodules/core"

# Push only those repos
./smart-commit-push.sh --provider copilot --model gpt-4o --repos ".,submodules/core"
```

## Troubleshooting

### AI Provider Not Found
```bash
# Check available providers
./smart-commit.sh --list-models

# Install missing provider
npm install -g @githubnext/github-copilot-cli  # Copilot
npm install -g @openai/codex                    # Codex
# OpenCode: https://opencode.ai/docs/cli/
```

### Model Cache Issues
```bash
# Clear all caches
./smart-commit.sh --clear-cache

# Clear specific provider
./smart-commit.sh --clear-cache copilot
```

### Conflict Resolution Failed
```bash
# Try interactive mode
./smart-resolve.sh --provider copilot --model gpt-4o --interactive

# Or resolve manually
git status
# Edit conflicted files
git add <files>
git rebase --continue
```

### Rebase Conflicts
```bash
# Abort and try with different strategy
git rebase --abort
./smart-rebase.sh --provider copilot --model gpt-4o --strategy ours
```

## Best Practices

1. **Start with dry-run** - Always test with `--dry-run` first
2. **Use interactive mode** - Review AI suggestions before applying
3. **Keep backups** - Don't delete `.conflict-backup` files immediately
4. **Custom rules** - Define team commit conventions
5. **Repository filtering** - Use `--repos` for targeted operations
6. **Model selection** - Choose appropriate model for task complexity
7. **Cache management** - Clear cache if models seem outdated

## Future Enhancements

Planned tools (not yet implemented):
- `smart-merge.sh` - AI-assisted merge operations
- `smart-cherry-pick.sh` - Intelligent cherry-pick
- `smart-squash.sh` - Smart commit squashing
- `smart-review.sh` - AI code review
- `smart-branch.sh` - Branch management

## See Also

- [Common Pitfalls](./common-pitfalls.md)
- [Commit Rules Examples](../examples/commit-rules-example.md)
- [Contributing](../development/contributing.md)
