# Manual Testing Guide

This guide provides step-by-step instructions for manually testing all Git Master Skill scripts with the demo repository.

## Prerequisites

1. Clone the test repository:
```bash
git clone git@github.com:dorgonman/kano-git-master-skill-demo.git /tmp/test-repo
cd /tmp/test-repo
```

2. Set skill root:
```bash
SKILL_ROOT="/path/to/skills/kano-git-master-skill"
```

## Test 1: update-repo.sh

Test updating a repository and its submodules.

```bash
# Test dry-run
bash $SKILL_ROOT/scripts/repo-management/update-repo.sh /tmp/test-repo --dry-run

# Test actual update
bash $SKILL_ROOT/scripts/repo-management/update-repo.sh /tmp/test-repo

# Verify
git -C /tmp/test-repo status
```

**Expected:** Repository updated, submodules updated, no errors.

## Test 2: clone-with-upstream.sh

Test cloning with upstream remote.

```bash
# Clone without upstream
bash $SKILL_ROOT/scripts/repo-management/clone-with-upstream.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --dir /tmp/test-clone

# Verify
cd /tmp/test-clone
git remote -v
```

**Expected:** Repository cloned, origin remote set.

```bash
# Clone with upstream
bash $SKILL_ROOT/scripts/repo-management/clone-with-upstream.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --dir /tmp/test-clone-upstream

# Verify
cd /tmp/test-clone-upstream
git remote -v
```

**Expected:** Repository cloned, origin and upstream remotes set.

## Test 3: discover-repos.sh

Test repository discovery.

```bash
# Create test workspace
mkdir -p /tmp/test-workspace
git clone git@github.com:dorgonman/kano-git-master-skill-demo.git /tmp/test-workspace/repo1
git clone git@github.com:dorgonman/kano-git-master-skill-demo.git /tmp/test-workspace/repo2

# Test discovery
bash $SKILL_ROOT/scripts/repo-management/discover-repos.sh \
  --root /tmp/test-workspace

# Test JSON output
bash $SKILL_ROOT/scripts/repo-management/discover-repos.sh \
  --root /tmp/test-workspace \
  --format json

# Test manifest save
bash $SKILL_ROOT/scripts/repo-management/discover-repos.sh \
  --root /tmp/test-workspace \
  --save /tmp/test-workspace/manifest.json

# Verify
cat /tmp/test-workspace/manifest.json
```

**Expected:** 2 repositories discovered, JSON output valid, manifest created.

## Test 4: status-all-repos.sh

Test status reporting.

```bash
# Table format
bash $SKILL_ROOT/scripts/workspace/status-all-repos.sh \
  --root /tmp/test-workspace

# JSON format
bash $SKILL_ROOT/scripts/workspace/status-all-repos.sh \
  --root /tmp/test-workspace \
  --format json

# Markdown format
bash $SKILL_ROOT/scripts/workspace/status-all-repos.sh \
  --root /tmp/test-workspace \
  --format markdown \
  --output /tmp/status.md

# Verify
cat /tmp/status.md
```

**Expected:** Status displayed in all formats, markdown file created.

## Test 5: foreach-repo.sh

Test command execution across repos.

```bash
# Check status
bash $SKILL_ROOT/scripts/workspace/foreach-repo.sh \
  "git status --short" \
  --root /tmp/test-workspace

# Check branches
bash $SKILL_ROOT/scripts/workspace/foreach-repo.sh \
  "git branch -a" \
  --root /tmp/test-workspace

# Fetch all
bash $SKILL_ROOT/scripts/workspace/foreach-repo.sh \
  "git fetch --all --prune" \
  --root /tmp/test-workspace
```

**Expected:** Commands executed in all repos, output shown for each.

## Test 6: update-workspace-repos.sh

Test batch updates.

```bash
# Dry-run
bash $SKILL_ROOT/scripts/workspace/update-workspace-repos.sh \
  --root /tmp/test-workspace \
  --dry-run

# Actual update
bash $SKILL_ROOT/scripts/workspace/update-workspace-repos.sh \
  --root /tmp/test-workspace

# With manifest
bash $SKILL_ROOT/scripts/workspace/update-workspace-repos.sh \
  --manifest /tmp/test-workspace/manifest.json
```

**Expected:** All repos updated, summary shown.

## Test 7: compare-branches.sh

Test branch comparison.

```bash
cd /tmp/test-repo

# Create test branch
git checkout -b test-branch
echo "test" > test-file.txt
git add test-file.txt
git commit -m "test: Add test file"

# Compare branches (table)
bash $SKILL_ROOT/scripts/branch-operations/compare-branches.sh \
  main test-branch \
  --repo /tmp/test-repo

# Compare branches (JSON)
bash $SKILL_ROOT/scripts/branch-operations/compare-branches.sh \
  main test-branch \
  --repo /tmp/test-repo \
  --format json

# Compare branches (markdown)
bash $SKILL_ROOT/scripts/branch-operations/compare-branches.sh \
  main test-branch \
  --repo /tmp/test-repo \
  --format markdown \
  --output /tmp/branch-diff.md

# Bidirectional comparison
bash $SKILL_ROOT/scripts/branch-operations/compare-branches.sh \
  main test-branch \
  --repo /tmp/test-repo \
  --bidirectional

# Verify
cat /tmp/branch-diff.md
```

**Expected:** Differences shown in all formats, markdown file created.

## Test 8: cherry-pick-batch.sh

Test batch cherry-pick.

```bash
cd /tmp/test-repo

# Create test commits
git checkout main
echo "commit1" > file1.txt
git add file1.txt
git commit -m "test: Commit 1"
HASH1=$(git rev-parse HEAD)

echo "commit2" > file2.txt
git add file2.txt
git commit -m "test: Commit 2"
HASH2=$(git rev-parse HEAD)

# Create target branch
git checkout -b cherry-pick-target HEAD~2

# Create JSON file
cat > /tmp/commits.json <<EOF
{
  "commits": [
    {
      "hash": "$HASH1",
      "title": "test: Commit 1"
    },
    {
      "hash": "$HASH2",
      "title": "test: Commit 2"
    }
  ]
}
EOF

# Test dry-run
bash $SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh \
  /tmp/commits.json \
  --repo /tmp/test-repo \
  --dry-run

# Test actual cherry-pick
bash $SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh \
  /tmp/commits.json \
  --repo /tmp/test-repo

# Verify
ls -la /tmp/test-repo/
git -C /tmp/test-repo log --oneline -5
```

**Expected:** Commits cherry-picked, files exist, git log shows commits.

### Test text format

```bash
cd /tmp/test-repo
git checkout -b text-format-test HEAD~2

# Create text file
cat > /tmp/commits.txt <<EOF
$HASH1 test: Commit 1
$HASH2 test: Commit 2
EOF

# Cherry-pick
bash $SKILL_ROOT/scripts/branch-operations/cherry-pick-batch.sh \
  /tmp/commits.txt \
  --repo /tmp/test-repo

# Verify
ls -la /tmp/test-repo/
```

**Expected:** Commits cherry-picked from text file.

## Test 9: rebase-to-upstream-latest.sh

Test rebasing to upstream.

```bash
cd /tmp/test-repo

# Add upstream remote
git remote add upstream git@github.com:dorgonman/kano-git-master-skill-demo.git
git fetch upstream

# Test dry-run
bash $SKILL_ROOT/scripts/branch-operations/rebase-to-upstream-latest.sh \
  --dry-run

# Test actual rebase (if safe)
# bash $SKILL_ROOT/scripts/branch-operations/rebase-to-upstream-latest.sh
```

**Expected:** Dry-run shows what would happen, actual rebase works if executed.

## Test 10: smart-commit.sh

Test AI-powered commit (requires GitHub Copilot CLI).

```bash
cd /tmp/test-repo

# Make some changes
echo "new feature" > new-file.txt
git add new-file.txt

# Test help
bash $SKILL_ROOT/scripts/commit-tools/smart-commit.sh --help

# Test with custom message (no AI needed)
bash $SKILL_ROOT/scripts/commit-tools/smart-commit.sh \
  -m "test: Add new file"

# Test with AI (requires Copilot)
# bash $SKILL_ROOT/scripts/commit-tools/smart-commit.sh

# Test dry-run with AI review disabled
# bash $SKILL_ROOT/scripts/commit-tools/smart-commit.sh \
#   --no-ai-review \
#   -m "test: Another change"
```

**Expected:** Help works, custom message commits work, AI features require Copilot.

## Cleanup

```bash
rm -rf /tmp/test-repo
rm -rf /tmp/test-clone
rm -rf /tmp/test-clone-upstream
rm -rf /tmp/test-workspace
rm -f /tmp/status.md
rm -f /tmp/branch-diff.md
rm -f /tmp/commits.json
rm -f /tmp/commits.txt
```

## Troubleshooting

### SSH Authentication Failed

Use HTTPS instead:
```bash
git clone https://github.com/dorgonman/kano-git-master-skill-demo.git /tmp/test-repo
```

### Permission Denied

Make scripts executable:
```bash
chmod +x $SKILL_ROOT/scripts/**/*.sh
```

### Script Not Found

Check skill root path:
```bash
echo $SKILL_ROOT
ls -la $SKILL_ROOT/scripts/
```

### Git Command Failed

Check git version:
```bash
git --version
```

Minimum required: Git 2.x

## Notes

- All tests use `/tmp/` directory for isolation
- Tests are non-destructive to your actual repositories
- Some tests require network access to clone repositories
- AI features require GitHub Copilot CLI installation
- Clean up test directories after testing

## Success Criteria

All tests should:
- ✓ Execute without errors
- ✓ Produce expected output
- ✓ Create expected files
- ✓ Show correct git status
- ✓ Handle edge cases gracefully

If any test fails, check:
1. Script permissions
2. Git installation
3. Network connectivity
4. Repository accessibility
5. Disk space
