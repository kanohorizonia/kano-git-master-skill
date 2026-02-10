---
name: kano-git-master-skill
description: Multi-repository git hygiene and commit orchestration with static safety checks and AI review gate. Use when preparing commits across root repo, submodules, and nested git repos, especially to prevent secret leaks, enforce .gitignore hygiene, and generate commit messages with Codex/Copilot fallback.
---

# Kano Git Master Skill

Use this skill to perform safe, repeatable commits across many repos under one workspace.

## Workflow

1. Run:
   - `bash scripts/git/ai-safe-commit-all-repos.sh --help`
2. Choose AI mode:
   - Default: `--ai auto` (Codex first, then Copilot)
   - Fixed provider: `--ai codex` or `--ai copilot`
   - No AI messaging: `--ai none` (only valid with `--no-ai-review`)
3. Keep AI gate enabled by default:
   - `--ai-review` (default)
   - If both AI providers are unavailable, explicitly choose `--no-ai-review`
4. Start without push:
   - `bash scripts/git/ai-safe-commit-all-repos.sh`
5. Push only when ready:
   - `bash scripts/git/ai-safe-commit-all-repos.sh -f`

## What The Script Enforces

- Enumerate commit targets:
  - Root repo
  - `.gitmodules` submodules
  - Any nested `.git` repo under workspace (including private repos not listed in `.gitmodules`)
- Auto-update `.gitignore` for common local-only files and nested-repo folders.
- Block commit on:
  - Conflict markers / bad whitespace in staged diff
  - Secret-like staged content (tokens, keys, passwords)
  - Secret-like files (`.env*`, `*.pem`, `*.key`, etc.)
  - Oversized staged files (default 5 MB)
- Run AI PASS/FAIL safety review (fail-closed by default).
- Generate commit message via AI provider with deterministic fallback.

## Script Location

- Main implementation:
  - `skills/kano-git-master-skill/scripts/ai-safe-commit-all-repos.sh`
- Wrapper entrypoint for convenience:
  - `scripts/git/ai-safe-commit-all-repos.sh`

## Related Utilities

- Rebase local branches to latest `origin/main` (root + submodules):
  - `scripts/git/rebase-to-latest-main.sh --help`
