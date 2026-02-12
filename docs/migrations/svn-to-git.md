# Subversion to Git Migration Guide

**Last Updated**: 2026-02-12

## Overview

This guide covers migrating from Subversion (SVN) to Git, including gradual migration strategies, hybrid workflows, and best practices.

## Migration Strategies

### Strategy 1: Big Bang Migration
- **Timeline**: 1-2 weeks
- **Risk**: High
- **Effort**: Medium
- **Best For**: Small teams, small repositories

### Strategy 2: Gradual Migration
- **Timeline**: 3-6 months
- **Risk**: Low
- **Effort**: High
- **Best For**: Large teams, large repositories

### Strategy 3: Hybrid Workflow
- **Timeline**: Ongoing
- **Risk**: Medium
- **Effort**: Medium
- **Best For**: Mixed teams, transition period

## Prerequisites

### Tools Required

```bash
# Git 2.x or higher
git --version

# git-svn
git svn --version

# Subversion client (svn)
svn --version
```

### Environment Setup

```bash
# Verify SVN connection
svn info https://svn.example.com/repo

# Test SVN credentials
svn list https://svn.example.com/repo
```

## Phase 1: Assessment

### Analyze Repository

```bash
# Check repository size
svn info https://svn.example.com/repo | grep "Repository Size"

# Count files
svn list -R https://svn.example.com/repo | wc -l

# Check history depth
svn log https://svn.example.com/repo | grep "^r" | wc -l

# Check repository structure
svn list https://svn.example.com/repo
```

### Identify Repository Layout

**Standard Layout**:
```
repo/
├── trunk/          # Main development
├── branches/       # Feature/release branches
└── tags/           # Release tags
```

**Non-Standard Layout**:
```
repo/
├── main/           # Main development
├── feature/        # Feature branches
└── releases/       # Release tags
```

**Single Branch**:
```
repo/               # No trunk/branches/tags
├── src/
└── docs/
```

### Identify Challenges

1. **Large Binary Files**
   - Consider Git LFS
   - Or keep in SVN

2. **Complex Branch Structure**
   - Map SVN branches to Git branches
   - Document branch strategy

3. **Large History**
   - Consider shallow clone
   - Or import recent history only

4. **Externals**
   - SVN externals don't map to Git
   - Convert to submodules or subtrees

## Phase 2: Planning

### Migration Plan Template

```markdown
# SVN to Git Migration Plan

## Timeline
- Week 1: Setup and testing
- Week 2: Pilot migration
- Week 3: Team training
- Week 4: Full migration
- Week 5: Cleanup and optimization

## Team
- Migration Lead: [Name]
- Git Expert: [Name]
- SVN Admin: [Name]
- Team Members: [Names]

## Repositories
- Source: https://svn.example.com/repo
- Target: https://github.com/company/myproject.git

## Layout
- Type: Standard (trunk/branches/tags)
- Trunk: trunk
- Branches: branches
- Tags: tags

## Branches to Migrate
- trunk → main
- branches/develop → develop
- branches/release-* → release/*
- branches/feature-* → feature/*

## Tags to Migrate
- tags/v1.0 → v1.0
- tags/v2.0 → v2.0

## Risks
- Data loss: Mitigated by backups
- Downtime: Mitigated by hybrid workflow
- Team confusion: Mitigated by training

## Rollback Plan
- Keep SVN read-only for 1 month
- Document rollback procedure
- Test rollback before migration
```

### Author Mapping

SVN uses different author format than Git. Create author mapping file:

```bash
# Create authors.txt
cat > authors.txt << 'EOF'
jsmith = John Smith <john.smith@example.com>
mjones = Mary Jones <mary.jones@example.com>
bwilson = Bob Wilson <bob.wilson@example.com>
EOF
```

To generate authors list from SVN:

```bash
# Extract all SVN authors
svn log -q https://svn.example.com/repo | \
  awk -F '|' '/^r/ {sub("^ ", "", $2); sub(" $", "", $2); print $2}' | \
  sort -u > authors-list.txt

# Convert to authors.txt format
while read author; do
  echo "$author = $author <$author@example.com>"
done < authors-list.txt > authors.txt
```

## Phase 3: Initial Clone

### Clone with Standard Layout

```bash
# Clone with standard layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo

# Or with authors file
git svn clone --authors-file=authors.txt \
  --trunk=trunk --branches=branches --tags=tags \
  https://svn.example.com/repo myproject

cd myproject
```

### Clone with Custom Layout

```bash
# Clone with custom layout
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo \
  --trunk main --branches feature --tags releases

# Or manually
git svn clone --authors-file=authors.txt \
  --trunk=main --branches=feature --tags=releases \
  https://svn.example.com/repo myproject
```

### Clone without Standard Layout

```bash
# Clone single branch
./scripts/vcs-bridges/svn/clone.sh https://svn.example.com/repo/trunk --no-standard

# Or manually
git svn clone --authors-file=authors.txt \
  https://svn.example.com/repo/trunk myproject
```

### Verify Clone

```bash
# Check Git history
git log --oneline | head -20

# Check branches
git branch -a

# Check tags
git tag

# Check file count
git ls-files | wc -l

# Check repository size
du -sh .git

# Verify git-svn metadata
git log -1 --format=%B
```

## Phase 4: Cleanup and Optimization

### Convert SVN Tags to Git Tags

```bash
# SVN tags are imported as branches
# Convert them to proper Git tags

# List SVN tag branches
git branch -r | grep "tags/"

# Convert each tag
for tag in $(git branch -r | grep "tags/" | sed 's/.*tags\///'); do
  git tag "$tag" "refs/remotes/origin/tags/$tag"
  git branch -r -d "origin/tags/$tag"
done
```

### Convert SVN Branches to Git Branches

```bash
# SVN branches are imported as remote branches
# Convert them to local branches

# List SVN branches
git branch -r | grep -v "tags/"

# Convert each branch
for branch in $(git branch -r | grep -v "tags/" | grep -v "trunk" | sed 's/.*origin\///'); do
  git branch "$branch" "refs/remotes/origin/$branch"
done
```

### Handle SVN Externals

SVN externals don't map directly to Git. Options:

**Option 1: Convert to Git Submodules**
```bash
# For each SVN external, add as submodule
git submodule add https://github.com/external/lib.git lib/external
```

**Option 2: Convert to Git Subtrees**
```bash
# For each SVN external, add as subtree
./scripts/subtree/add-subtree.sh \
  --prefix lib/external \
  --url https://github.com/external/lib.git
```

**Option 3: Vendor the Code**
```bash
# Copy external code directly into repository
# (Simplest but loses connection to upstream)
```

### Remove Large Files (Optional)

```bash
# Find large files
git rev-list --objects --all | \
  git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' | \
  awk '/^blob/ {print substr($0,6)}' | \
  sort --numeric-sort --key=2 | \
  tail -20

# Remove large files from history (use BFG Repo-Cleaner)
# https://rtyley.github.io/bfg-repo-cleaner/
```

### Optimize Repository

```bash
# Run garbage collection
git gc --aggressive --prune=now

# Repack objects
git repack -a -d -f --depth=250 --window=250

# Check final size
du -sh .git
```

## Phase 5: Setup Git Repository

### Create Remote Repository

```bash
# On GitHub/GitLab/Bitbucket
# Create new repository: myproject

# Add remote
git remote add origin https://github.com/company/myproject.git

# Rename master to main (if needed)
git branch -M main

# Push all branches
git push -u origin --all

# Push all tags
git push -u origin --tags
```

### Configure Repository

```bash
# Add .gitignore
cat > .gitignore << 'EOF'
# Build outputs
/build/
/dist/
*.o
*.exe

# IDE files
.vscode/
.idea/
*.swp

# OS files
.DS_Store
Thumbs.db

# SVN files (if any remain)
.svn/
EOF

git add .gitignore
git commit -m "Add .gitignore"
git push
```

### Configure Branch Protection

```bash
# In GitHub/GitLab/Bitbucket UI:
# - Protect main branch
# - Require pull request reviews
# - Require status checks
# - Require linear history
```

## Phase 6: Hybrid Workflow (Transition Period)

### Setup Bidirectional Sync

```bash
# Keep git-svn configuration
# Don't remove git-svn metadata yet

# Fetch from SVN regularly
./scripts/vcs-bridges/svn/fetch.sh

# Rebase on SVN changes
./scripts/vcs-bridges/svn/rebase.sh

# Commit to SVN when needed
./scripts/vcs-bridges/svn/dcommit.sh
```

### Team Workflow During Transition

**Git Users**:
```bash
# Clone from Git
git clone https://github.com/company/myproject.git

# Work in Git
git checkout -b feature-branch
# ... make changes ...
git commit -m "Add feature"
git push origin feature-branch

# Create pull request
# After merge, changes sync to SVN via CI/CD
```

**SVN Users**:
```bash
# Continue using SVN
svn checkout https://svn.example.com/repo/trunk
# ... make changes ...
svn commit -m "Add feature"

# Changes sync to Git via CI/CD
```

### Automated Sync (CI/CD)

```yaml
# .github/workflows/svn-sync.yml
name: SVN Sync

on:
  schedule:
    - cron: '*/30 * * * *'  # Every 30 minutes

jobs:
  sync:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
        with:
          fetch-depth: 0
      
      - name: Setup git-svn
        run: |
          sudo apt-get install git-svn
          
      - name: Fetch from SVN
        run: |
          ./scripts/vcs-bridges/svn/fetch.sh
          
      - name: Rebase on SVN
        run: |
          ./scripts/vcs-bridges/svn/rebase.sh
          
      - name: Push to Git
        run: |
          git push origin main
```

## Phase 7: Team Training

### Training Topics

1. **Git Basics**
   - Clone, commit, push, pull
   - Branching and merging
   - Resolving conflicts

2. **Git Workflow**
   - Feature branch workflow
   - Pull requests
   - Code review

3. **Differences from SVN**
   - Distributed vs centralized
   - Local commits
   - Branching model

4. **Git Tools**
   - Command line
   - GUI clients (GitKraken, SourceTree)
   - IDE integration

### Training Resources

```markdown
# Git Training Resources

## Online Courses
- [Git Basics](https://git-scm.com/book/en/v2)
- [GitHub Learning Lab](https://lab.github.com/)
- [Atlassian Git Tutorials](https://www.atlassian.com/git/tutorials)

## Cheat Sheets
- [Git Cheat Sheet](https://education.github.com/git-cheat-sheet-education.pdf)
- [SVN to Git Command Mapping](./SVN-TO-GIT-COMMANDS.md)

## Internal Resources
- Team Wiki: [Git Workflow]
- Slack Channel: #git-help
- Office Hours: Tuesdays 2-3pm
```

### SVN to Git Command Mapping

| SVN Command | Git Equivalent |
|-------------|----------------|
| `svn checkout` | `git clone` |
| `svn update` | `git pull` |
| `svn commit` | `git commit` + `git push` |
| `svn add` | `git add` |
| `svn delete` | `git rm` |
| `svn move` | `git mv` |
| `svn status` | `git status` |
| `svn diff` | `git diff` |
| `svn log` | `git log` |
| `svn revert` | `git checkout` or `git restore` |
| `svn merge` | `git merge` |
| `svn copy` (branch) | `git branch` |
| `svn switch` | `git checkout` |

## Phase 8: Full Migration

### Cutover Plan

```markdown
# Cutover Checklist

## Pre-Cutover (1 week before)
- [ ] Final fetch from SVN
- [ ] Verify all branches migrated
- [ ] Verify all tags migrated
- [ ] Test Git workflows
- [ ] Train all team members
- [ ] Prepare rollback plan

## Cutover Day
- [ ] Announce cutover to team
- [ ] Make SVN read-only
- [ ] Final fetch from SVN
- [ ] Final rebase on SVN
- [ ] Remove git-svn configuration
- [ ] Push to Git
- [ ] Update CI/CD pipelines
- [ ] Update documentation
- [ ] Announce completion

## Post-Cutover (1 week after)
- [ ] Monitor for issues
- [ ] Provide support to team
- [ ] Collect feedback
- [ ] Optimize workflows
- [ ] Archive SVN (after 1 month)
```

### Make SVN Read-Only

```bash
# On SVN server, edit authz file
# Add this at the top:
[/]
* = r

# This makes the repository read-only for all users
```

### Final Sync and Cleanup

```bash
# Final fetch from SVN
./scripts/vcs-bridges/svn/fetch.sh

# Final rebase
./scripts/vcs-bridges/svn/rebase.sh

# Remove git-svn configuration
git config --remove-section svn-remote.svn

# Remove git-svn refs
rm -rf .git/svn

# Push to Git
git push origin --all
git push origin --tags
```

## Phase 9: Post-Migration

### Update CI/CD Pipelines

```yaml
# Before (SVN)
- name: Checkout
  run: svn checkout https://svn.example.com/repo/trunk

# After (Git)
- name: Checkout
  uses: actions/checkout@v2
```

### Update Documentation

```markdown
# Update all references to SVN

## Before
- Clone: svn checkout https://svn.example.com/repo/trunk
- Commit: svn commit -m "message"

## After
- Clone: git clone https://github.com/company/myproject.git
- Commit: git commit -m "message" && git push
```

### Archive SVN

```bash
# After 1 month of successful Git usage

# Create final backup
svnadmin dump /path/to/repo > repo-backup.dump

# Archive repository
tar -czf repo-archive-2026-02-12.tar.gz /path/to/repo

# Document archive location
echo "SVN archive: /backups/svn/repo-archive-2026-02-12.tar.gz" >> MIGRATION.md
```

## Troubleshooting

### Common Issues

**Issue: Clone is very slow**
```bash
# Solution: Clone recent history only
git svn clone -r 1000:HEAD https://svn.example.com/repo

# Or use shallow clone
git svn clone --depth 1 https://svn.example.com/repo
```

**Issue: Authors not mapped**
```bash
# Solution: Create authors.txt file
# See "Author Mapping" section above

# Re-clone with authors file
git svn clone --authors-file=authors.txt https://svn.example.com/repo
```

**Issue: SVN externals not migrated**
```bash
# Solution: Manually convert to submodules or subtrees
# See "Handle SVN Externals" section above
```

**Issue: Large repository size**
```bash
# Solution: Use Git LFS for large files
git lfs install
git lfs track "*.psd"
git lfs track "*.zip"
git add .gitattributes
git commit -m "Add Git LFS tracking"
```

**Issue: Tags not created**
```bash
# Solution: Manually convert SVN tag branches to Git tags
# See "Convert SVN Tags to Git Tags" section above
```

## Best Practices

### During Migration

1. **Backup Everything**: Backup SVN before migration
2. **Test First**: Test migration on small project first
3. **Map Authors**: Create complete authors mapping
4. **Communicate**: Keep team informed throughout
5. **Document**: Document all decisions and procedures
6. **Support**: Provide support during transition

### After Migration

1. **Monitor**: Monitor for issues in first month
2. **Optimize**: Optimize Git workflows based on feedback
3. **Train**: Continue training team on Git
4. **Archive**: Archive SVN after successful migration
5. **Celebrate**: Celebrate successful migration!

## Rollback Plan

### If Migration Fails

```bash
# 1. Announce rollback
echo "Rolling back to SVN"

# 2. Make SVN writable again
# Edit authz file to restore write permissions

# 3. Update from SVN
svn update

# 4. Resume SVN workflows
# Team continues using SVN

# 5. Analyze failure
# Document what went wrong
# Plan fixes for next attempt
```

## Success Criteria

### Migration is Successful If:

- ✅ All code migrated to Git
- ✅ All history preserved
- ✅ All branches migrated
- ✅ All tags migrated
- ✅ All team members trained
- ✅ CI/CD pipelines updated
- ✅ No data loss
- ✅ Team productive in Git
- ✅ SVN archived

## Comparison: SVN vs Git

### Advantages of Git over SVN

1. **Distributed**: Every developer has full repository
2. **Fast**: Most operations are local
3. **Branching**: Cheap and easy branching
4. **Offline Work**: Commit without network
5. **Better Merging**: Superior merge algorithms
6. **Staging Area**: Review changes before commit
7. **Community**: Larger community and ecosystem

### What You'll Miss from SVN

1. **Simplicity**: SVN is simpler to learn
2. **Revision Numbers**: Sequential revision numbers
3. **Partial Checkout**: SVN allows partial checkout
4. **Centralized**: Single source of truth
5. **File Locking**: SVN supports file locking

## Conclusion

Migrating from SVN to Git is a significant undertaking, but with proper planning and execution, it can be done successfully. The key is to:

1. **Plan thoroughly**: Assess, plan, and prepare
2. **Test extensively**: Test migration before cutover
3. **Train team**: Ensure everyone is comfortable with Git
4. **Support transition**: Provide support during and after migration
5. **Monitor closely**: Watch for issues and address quickly

---

**See Also**:
- [Git-SVN Scripts Documentation](../scripts/vcs-bridges/svn/)
- [Git-P4 vs Git-SVN Comparison](./GIT-P4-VS-GIT-SVN.md)
- [SVN to Git Command Mapping](./SVN-TO-GIT-COMMANDS.md) (to be created)
