Generate one concise commit message for this git change.

Rules:
- Output exactly one line, no markdown, no quotes.
- Prefer Conventional Commits: type(scope): summary.
- Allowed types: feat, fix, refactor, chore, docs, test, ci, build, perf, style.
- Keep summary under 72 characters when possible.

Repository context:
- Repo: {{REPO_LABEL}}
- Staged: {{STAGED_COUNT}}
- Unstaged: {{UNSTAGED_COUNT}}
- Untracked: {{UNTRACKED_COUNT}}

Staged diff:
{{STAGED_DIFF}}

