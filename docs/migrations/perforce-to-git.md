# Perforce to Git Migration Guide

**Last Updated**: 2026-02-12

## Overview

This guide covers migrating from Perforce to Git, including gradual migration strategies, hybrid workflows, and best practices.

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

# Python 3.x (for git-p4)
python3 --version

# git-p4
git p4 --help

# Perforce client (p4)
p4 -V
```

### Environment Setup

```bash
# Set Perforce environment variables
export P4PORT=perforce:1666
export P4USER=your-username
export P4CLIENT=your-workspace

# Verify connection
p4 info
```

## Phase 1: Assessment

### Analyze Repository

```bash
# Check repository size
p4 sizes //depot/myproject/...

# Count files
p4 files //depot/myproject/... | wc -l

# Check history depth
p4 changes //depot/myproject/... | wc -l

# Identify large files
p4 sizes -s //depot/myproject/... | sort -n -r | head -20
```

### Identify Challenges

1. **Large Binary Files**
   - Consider Git LFS
   - Or keep in Perforce

2. **Complex Branch Structure**
   - Map Perforce branches to Git branches
   - Document branch strategy

3. **Large History**
   - Consider shallow clone
   - Or import recent history only

4. **Active Development**
   - Plan migration during quiet period
   - Coordinate with team

## Phase 2: Planning

### Migration Plan Template

```markdown
# Perforce to Git Migration Plan

## Timeline
- Week 1: Setup and testing
- Week 2: Pilot migration
- Week 3: Team training
- Week 4: Full migration
- Week 5: Cleanup and optimization

## Team
- Migration Lead: [Name]
- Git Expert: [Name]
- Perforce Admin: [Name]
- Team Members: [Names]

## Repositories
- Source: //depot/myproject/...
- Target: https://github.com/company/myproject.git

## Branches
- main: //depot/myproject/main/...
- develop: //depot/myproject/dev/...
- release: //depot/myproject/release/...

## Risks
- Data loss: Mitigated by backups
- Downtime: Mitigated by hybrid workflow
- Team confusion: Mitigated by training

## Rollback Plan
- Keep Perforce read-only for 1 month
- Document rollback procedure
- Test rollback before migration
```

### Branch Mapping

```bash
# Map Perforce depot paths to Git branches
//depot/myproject/main/...     → main
//depot/myproject/dev/...      → develop
//depot/myproject/release/...  → release/*
//depot/myproject/feature/...  → feature/*
```

## Phase 3: Initial Clone

### Clone from Perforce

```bash
# Clone entire depot
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/...

# Or clone specific branch
./scripts/vcs-bridges/p4/clone.sh //depot/myproject/main/... --branch main

# Navigate to repository
cd myproject
```

### Verify Clone

```bash
# Check Git history
git log --oneline | head -20

# Check file count
git ls-files | wc -l

# Check repository size
du -sh .git

# Verify git-p4 metadata
git log -1 --format=%B
```

## Phase 4: Cleanup and Optimization

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

### Strip Git-P4 Metadata

```bash
# Strip metadata from all commits
./scripts/vcs-bridges/p4/strip-metadata.sh --all

# Or strip from specific range
./scripts/vcs-bridges/p4/strip-metadata.sh HEAD~100..HEAD
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

# Push all branches
git push -u origin --all

# Push all tags
git push -u origin --tags
```

### Configure Repository

```bash
# Set default branch
git branch -M main

# Configure branch protection
# (Do this in GitHub/GitLab/Bitbucket UI)

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
EOF

git add .gitignore
git commit -m "Add .gitignore"
git push
```

## Phase 6: Hybrid Workflow (Transition Period)

### Setup Bidirectional Sync

```bash
# Keep git-p4 configuration
# Don't strip metadata yet

# Sync from Perforce regularly
./scripts/vcs-bridges/p4/sync.sh --rebase

# Submit to Perforce when needed
./scripts/vcs-bridges/p4/submit.sh
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
# After merge, changes sync to Perforce via CI/CD
```

**Perforce Users**:
```bash
# Continue using Perforce
p4 sync //depot/myproject/...
# ... make changes ...
p4 submit

# Changes sync to Git via CI/CD
```

### Automated Sync (CI/CD)

```yaml
# .github/workflows/p4-sync.yml
name: Perforce Sync

on:
  schedule:
    - cron: '*/30 * * * *'  # Every 30 minutes

jobs:
  sync:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Setup git-p4
        run: |
          sudo apt-get install git-p4
          
      - name: Sync from Perforce
        env:
          P4PORT: ${{ secrets.P4PORT }}
          P4USER: ${{ secrets.P4USER }}
          P4PASSWD: ${{ secrets.P4PASSWD }}
        run: |
          ./scripts/vcs-bridges/p4/sync.sh --rebase
          
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

3. **Git Tools**
   - Command line
   - GUI clients (GitKraken, SourceTree)
   - IDE integration

4. **Migration-Specific**
   - Differences from Perforce
   - New workflows
   - Troubleshooting

### Training Resources

```markdown
# Git Training Resources

## Online Courses
- [Git Basics](https://git-scm.com/book/en/v2)
- [GitHub Learning Lab](https://lab.github.com/)
- [Atlassian Git Tutorials](https://www.atlassian.com/git/tutorials)

## Cheat Sheets
- [Git Cheat Sheet](https://education.github.com/git-cheat-sheet-education.pdf)
- Perforce to Git command mapping (historical)

## Internal Resources
- Team Wiki: [Git Workflow]
- Slack Channel: #git-help
- Office Hours: Tuesdays 2-3pm
```

## Phase 8: Full Migration

### Cutover Plan

```markdown
# Cutover Checklist

## Pre-Cutover (1 week before)
- [ ] Final sync from Perforce
- [ ] Verify all branches migrated
- [ ] Test Git workflows
- [ ] Train all team members
- [ ] Prepare rollback plan

## Cutover Day
- [ ] Announce cutover to team
- [ ] Make Perforce read-only
- [ ] Final sync from Perforce
- [ ] Strip git-p4 metadata
- [ ] Push to Git
- [ ] Update CI/CD pipelines
- [ ] Update documentation
- [ ] Announce completion

## Post-Cutover (1 week after)
- [ ] Monitor for issues
- [ ] Provide support to team
- [ ] Collect feedback
- [ ] Optimize workflows
- [ ] Archive Perforce (after 1 month)
```

### Make Perforce Read-Only

```bash
# On Perforce server
p4 protect

# Add this line at the top:
# write user * * -//depot/myproject/...
# This makes the depot read-only for all users
```

### Final Sync and Cleanup

```bash
# Final sync from Perforce
./scripts/vcs-bridges/p4/sync.sh --rebase

# Strip all git-p4 metadata
./scripts/vcs-bridges/p4/strip-metadata.sh --all --force

# Push to Git
git push origin --all
git push origin --tags

# Remove git-p4 configuration
git config --remove-section git-p4
```

## Phase 9: Post-Migration

### Update CI/CD Pipelines

```yaml
# Before (Perforce)
- name: Checkout
  run: p4 sync //depot/myproject/...

# After (Git)
- name: Checkout
  uses: actions/checkout@v2
```

### Update Documentation

```markdown
# Update all references to Perforce

## Before
- Clone: p4 sync //depot/myproject/...
- Submit: p4 submit

## After
- Clone: git clone https://github.com/company/myproject.git
- Submit: git push
```

### Archive Perforce

```bash
# After 1 month of successful Git usage

# Create final backup
p4 admin checkpoint

# Archive depot
p4 admin archive //depot/myproject/...

# Document archive location
echo "Perforce archive: /backups/p4/myproject-2026-02-12.tar.gz" >> MIGRATION.md
```

## Troubleshooting

### Common Issues

**Issue: Clone is very slow**
```bash
# Solution: Use shallow clone
git p4 clone --depth 1 //depot/myproject/...

# Or clone recent history only
git p4 clone --max-changes 1000 //depot/myproject/...
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

**Issue: Python 3 not found**
```bash
# Solution: Install Python 3
# Windows: python.org
# macOS: brew install python3
# Linux: apt-get install python3
```

**Issue: Metadata in commits**
```bash
# Solution: Strip metadata
./scripts/vcs-bridges/p4/strip-metadata.sh --all
```

## Best Practices

### During Migration

1. **Backup Everything**: Backup Perforce before migration
2. **Test First**: Test migration on small project first
3. **Communicate**: Keep team informed throughout
4. **Document**: Document all decisions and procedures
5. **Support**: Provide support during transition

### After Migration

1. **Monitor**: Monitor for issues in first month
2. **Optimize**: Optimize Git workflows based on feedback
3. **Train**: Continue training team on Git
4. **Archive**: Archive Perforce after successful migration
5. **Celebrate**: Celebrate successful migration!

## Rollback Plan

### If Migration Fails

```bash
# 1. Announce rollback
echo "Rolling back to Perforce"

# 2. Make Perforce writable again
p4 protect
# Remove read-only restriction

# 3. Sync Perforce to latest
p4 sync //depot/myproject/...

# 4. Resume Perforce workflows
# Team continues using Perforce

# 5. Analyze failure
# Document what went wrong
# Plan fixes for next attempt
```

## Success Criteria

### Migration is Successful If:

- ✅ All code migrated to Git
- ✅ All history preserved
- ✅ All team members trained
- ✅ CI/CD pipelines updated
- ✅ No data loss
- ✅ Team productive in Git
- ✅ Perforce archived

## Conclusion

Migrating from Perforce to Git is a significant undertaking, but with proper planning and execution, it can be done successfully. The key is to:

1. **Plan thoroughly**: Assess, plan, and prepare
2. **Test extensively**: Test migration before cutover
3. **Train team**: Ensure everyone is comfortable with Git
4. **Support transition**: Provide support during and after migration
5. **Monitor closely**: Watch for issues and address quickly

---

**See Also**:
- [Git-P4 Guide](../guides/git-p4.md)
- [Git-P4 vs Git-SVN Comparison](../comparisons/git-p4-vs-git-svn.md)
- Perforce to Git command mapping (historical)
