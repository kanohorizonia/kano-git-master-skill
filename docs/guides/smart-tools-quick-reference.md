# Smart Git Tools - Quick Reference

Quick reference card for AI-powered Git tools.

## Quick Start

```bash
# Install AI provider (choose one)
npm install -g @githubnext/github-copilot-cli  # Copilot
npm install -g @openai/codex                    # Codex
# OpenCode: https://opencode.ai/docs/cli/

# List available models
./smart-commit.sh --list-models copilot
```

## Common Commands

### Commit
```bash
# Basic commit
./smart-commit-copilot.sh

# With custom rules
./smart-commit-copilot.sh --rules-file .github/commit-rules.md

# Only specific repos
./smart-commit-copilot.sh --repos ".,submodules/lib"
```

### Commit + Push
```bash
# Full workflow
./smart-commit-push.sh --provider copilot --model gpt-4o

# Dry run
./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run
```

### Resolve Conflicts
```bash
# Auto-resolve
./smart-resolve.sh --provider copilot --model gpt-4o

# Interactive (review each)
./smart-resolve.sh --provider copilot --model gpt-4o --interactive

# Specific file
./smart-resolve.sh --provider copilot --model gpt-4o --file src/main.ts
```

### Rebase
```bash
# Smart rebase
./smart-rebase.sh --provider copilot --model gpt-4o

# Onto specific branch
./smart-rebase.sh --provider copilot --model gpt-4o --onto main

# Interactive with AI
./smart-rebase.sh --provider copilot --model gpt-4o --interactive
```

## Tool Matrix

| Tool | Purpose | Key Features |
|------|---------|--------------|
| `smart-commit.sh` | AI commit messages | Safety checks, custom rules, auto-fix whitespace |
| `smart-commit-push.sh` | Complete workflow | Commit + fetch + rebase + push |
| `smart-resolve.sh` | Conflict resolution | Auto/interactive modes, backups |
| `smart-rebase.sh` | Intelligent rebase | AI strategy, auto-squash, conflict resolution |

## Common Flags

| Flag | Description | Example |
|------|-------------|---------|
| `--provider <name>` | AI provider | `--provider copilot` |
| `--model <name>` | AI model | `--model gpt-4o` |
| `--repos <paths>` | Filter repos | `--repos ".,sub/lib"` |
| `--dry-run` | Preview only | `--dry-run` |
| `--interactive` | Review mode | `--interactive` |
| `--no-ai-review` | Skip AI review | `--no-ai-review` |
| `--rules <text>` | Inline rules | `--rules "Use emoji"` |
| `--rules-file <path>` | Rules file | `--rules-file .github/rules.md` |

## Providers & Models

### OpenCode
```bash
--provider opencode --model auto
```

### Codex (OpenAI)
```bash
--provider codex --model gpt-5.3-codex
--provider codex --model gpt-4o
--provider codex --model o3
```

### Copilot (GitHub)
```bash
--provider copilot --model gpt-5-mini
--provider copilot --model gpt-4o
--provider copilot --model claude-4.5-sonnet
```

## Workflow Examples

### Daily Development
```bash
# 1. Make changes
vim src/main.ts

# 2. Commit
./smart-commit-copilot.sh

# 3. Push
./smart-commit-push.sh --provider copilot --model gpt-4o
```

### Fix Conflicts
```bash
# 1. Merge/rebase
git merge feature

# 2. Resolve conflicts
./smart-resolve.sh --provider copilot --model gpt-4o --interactive

# 3. Continue
git merge --continue
```

### Clean History
```bash
# 1. Rebase with AI
./smart-rebase.sh --provider copilot --model gpt-4o --interactive --auto-squash

# 2. Force push safely
git push --force-with-lease
```

## Troubleshooting

### Provider not found
```bash
# Check available
./smart-commit.sh --list-models

# Install missing
npm install -g @githubnext/github-copilot-cli
```

### Clear cache
```bash
# All providers
./smart-commit.sh --clear-cache

# Specific provider
./smart-commit.sh --clear-cache copilot
```

### Abort operations
```bash
# Abort merge
git merge --abort

# Abort rebase
git rebase --abort

# Abort cherry-pick
git cherry-pick --abort
```

## Safety Tips

1. ✅ Always use `--dry-run` first
2. ✅ Review AI suggestions in `--interactive` mode
3. ✅ Keep `.conflict-backup` files until verified
4. ✅ Use `--repos` to limit scope
5. ✅ Test on feature branches first
6. ⚠️ Don't trust AI blindly
7. ⚠️ Review generated commit messages
8. ⚠️ Verify conflict resolutions

## File Locations

```
scripts/commit-tools/
├── smart-commit.sh              # Main commit tool
├── smart-commit-push.sh         # Commit + push workflow
├── smart-resolve.sh             # Conflict resolution
├── smart-rebase.sh              # Intelligent rebase
├── smart-commit-copilot.sh      # Copilot wrapper
├── smart-commit-codex.sh        # Codex wrapper
├── smart-commit-opencode.sh     # OpenCode wrapper
└── lib/
    ├── ai-providers.sh          # AI abstraction
    ├── git-helpers.sh           # Git utilities
    └── conflict-parser.sh       # Conflict parsing
```

## Custom Rules

### Auto-detected locations
- `.git/commit-rules.md`
- `.github/commit-rules.md`
- `COMMIT_RULES.md`
- `.commit-rules`

### Example rule
```markdown
# Commit Rules

Format: type(scope): summary

Types:
- ✨ feat: New feature
- 🐛 fix: Bug fix
- 📝 docs: Documentation

Rules:
- Keep under 60 chars
- Use present tense
- No period at end
```

## Cache Location

```
~/.cache/kano-git-master-skill/models/
├── opencode.txt
├── codex.txt
└── copilot.txt
```

TTL: 24 hours

## See Also

- [Full Documentation](./smart-git-tools.md)
- [Common Pitfalls](./common-pitfalls.md)
- [Commit Rules Examples](../examples/commit-rules-example.md)
