# Commit Rules Example

This file shows how to customize commit message generation in `smart-commit.sh`.

## Usage

### Option 1: Auto-detection (Recommended)
Place this file in one of these locations:
- `.git/commit-rules.md` (repo-specific, not committed)
- `.github/commit-rules.md` (committed, shared with team)
- `COMMIT_RULES.md` (root of repo)
- `.commit-rules` (hidden file)

The script will automatically detect and use it.

### Option 2: Explicit file path
```bash
./smart-commit.sh --provider copilot --model gpt-5-mini --rules-file path/to/rules.md
```

### Option 3: Inline rules
```bash
./smart-commit.sh --provider copilot --model gpt-5-mini --rules "Use emoji prefixes"
```

## Example Rules

### Conventional Commits with Emoji
```markdown
# Commit Message Rules

Use Conventional Commits with emoji prefixes:

- ✨ feat: New feature
- 🐛 fix: Bug fix
- 📝 docs: Documentation changes
- ♻️ refactor: Code refactoring
- ✅ test: Test changes
- 🔧 chore: Maintenance tasks
- 🚀 perf: Performance improvements
- 💄 style: Code style changes
- 🔨 build: Build system changes
- 👷 ci: CI/CD changes

Examples:
- ✨ feat(auth): add OAuth2 support
- 🐛 fix(api): handle null response
- 📝 docs(readme): update installation steps

Rules:
- Keep summary under 60 characters
- Use present tense ("add" not "added")
- No period at the end
- Include scope when relevant
```

### Jira Ticket Format
```markdown
# Commit Message Rules

Format: [TICKET-ID] type: description

Types:
- feature: New functionality
- bugfix: Bug fixes
- hotfix: Critical fixes
- refactor: Code improvements
- docs: Documentation

Examples:
- [PROJ-123] feature: implement user authentication
- [PROJ-456] bugfix: fix memory leak in cache
- [PROJ-789] docs: update API documentation

Rules:
- Always include Jira ticket ID
- Keep description under 50 characters
- Use lowercase for type and description
```

### Semantic Release Format
```markdown
# Commit Message Rules

Follow Angular commit convention for semantic-release:

Format: type(scope): subject

Types (affects versioning):
- feat: Minor version bump (new feature)
- fix: Patch version bump (bug fix)
- perf: Patch version bump (performance)
- BREAKING CHANGE: Major version bump

Other types (no version bump):
- docs, style, refactor, test, chore, ci, build

Rules:
- Subject must be lowercase
- No period at end
- Use imperative mood ("add" not "adds" or "added")
- Scope is optional but recommended
- Body and footer are optional

Examples:
- feat(api): add user profile endpoint
- fix(auth): prevent token expiration race condition
- perf(db): optimize query performance
- docs(readme): clarify installation steps
```

### Custom Team Format
```markdown
# Commit Message Rules

Our team uses this format:

[Component] Action: What changed

Components:
- API: Backend API changes
- UI: Frontend UI changes
- DB: Database changes
- INFRA: Infrastructure changes
- TEST: Test changes
- DOC: Documentation

Actions:
- Add: New functionality
- Fix: Bug fixes
- Update: Modifications
- Remove: Deletions
- Refactor: Code improvements

Examples:
- [API] Add: user authentication endpoint
- [UI] Fix: button alignment on mobile
- [DB] Update: add index to users table
- [INFRA] Add: Docker configuration
```

## Tips

1. **Keep it simple**: AI works best with clear, concise rules
2. **Provide examples**: Examples help AI understand the format
3. **Be specific**: Specify character limits, case requirements, etc.
4. **Test it**: Try a few commits to see if the AI follows your rules
5. **Iterate**: Adjust rules based on the generated messages

## Priority

Rules are loaded in this order (first found wins):
1. `--rules` flag (inline text)
2. `--rules-file` flag (explicit path)
3. Auto-detected files (in order listed above)
4. Default Conventional Commits format
