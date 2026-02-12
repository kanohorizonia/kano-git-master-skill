# Testing Guide

Comprehensive testing documentation for Git Master Skill.

📊 **Visual Guide**: See [scripts/test/TEST-DIAGRAMS.md](scripts/test/TEST-DIAGRAMS.md) for Mermaid diagrams explaining test flows and scenarios.

## Quick Start

### 1. Quick Smoke Test (30 seconds)

Validates all scripts exist and help commands work:

```bash
cd skills/kano-git-master-skill
bash scripts/test/quick-test.sh
```

### 2. Full Automated Test Suite (5-10 minutes)

Runs comprehensive tests with real repository:

```bash
cd skills/kano-git-master-skill
bash scripts/test/run-all-tests.sh \
  --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --cleanup
```

### 3. Manual Testing (optional)

Follow step-by-step manual testing guide:

```bash
cat scripts/test/MANUAL-TESTING.md
```

## Test Files

```
scripts/test/
├── README.md              # Test suite documentation
├── MANUAL-TESTING.md      # Manual testing guide
├── quick-test.sh          # Quick smoke tests
├── run-all-tests.sh       # Full automated test suite
└── test-data/             # Test data files
    ├── cherry-pick-example.json
    └── cherry-pick-example.txt
```

## Test Coverage

### Repository Management (3 scripts)
- ✓ `update-repo.sh` - Update with dry-run and actual execution
- ✓ `clone-with-upstream.sh` - Clone with/without upstream
- ✓ `discover-repos.sh` - Discovery with multiple formats

### Workspace Operations (3 scripts)
- ✓ `status-all-repos.sh` - All output formats (table/JSON/markdown)
- ✓ `foreach-repo.sh` - Command execution across repos
- ✓ `update-workspace-repos.sh` - Batch updates with manifest

### Branch Operations (3 scripts)
- ✓ `compare-branches.sh` - All formats, bidirectional
- ✓ `cherry-pick-batch.sh` - JSON and text formats
- ✓ `rebase-to-upstream-latest.sh` - Dry-run mode

### Commit Tools (1 script)
- ✓ `smart-commit.sh` - Help output (full test requires Copilot)

### Helper Library
- ✓ `git-helpers.sh` - Function loading and availability

## Test Modes

### Quick Test
- **Duration**: ~30 seconds
- **Requirements**: None (no repository needed)
- **Coverage**: Script existence, help commands, library loading
- **Use case**: Pre-commit validation, quick sanity check

### Full Test Suite
- **Duration**: 5-10 minutes
- **Requirements**: Test repository access, Git 2.x+
- **Coverage**: All scripts with real operations
- **Use case**: CI/CD, release validation, comprehensive testing

### Manual Testing
- **Duration**: 30-60 minutes
- **Requirements**: Test repository, manual execution
- **Coverage**: All scripts with verification steps
- **Use case**: Development, debugging, learning

## Running Tests

### Local Development

```bash
# Quick check before commit
bash scripts/test/quick-test.sh

# Full test before push
bash scripts/test/run-all-tests.sh --cleanup
```

### CI/CD Integration

#### GitHub Actions

```yaml
name: Test Git Master Skill

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Quick test
        run: bash skills/kano-git-master-skill/scripts/test/quick-test.sh
      
      - name: Full test
        run: |
          bash skills/kano-git-master-skill/scripts/test/run-all-tests.sh \
            --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
            --cleanup
```

#### GitLab CI

```yaml
test:
  script:
    - bash skills/kano-git-master-skill/scripts/test/quick-test.sh
    - bash skills/kano-git-master-skill/scripts/test/run-all-tests.sh --cleanup
```

## Test Repository

**URL**: `git@github.com:dorgonman/kano-git-master-skill-demo.git`

Requirements:
- Publicly accessible or accessible with SSH key
- Has `main` branch
- Simple structure for testing
- Can be cloned multiple times

## Test Output Examples

### Quick Test Success

```
Git Master Skill - Quick Test
==============================

Test 1: Checking script files...
  ✓ Found: repo-management/update-repo.sh
  ✓ Found: repo-management/clone-with-upstream.sh
  ...

Test 2: Checking help output...
  ✓ Help works: repo-management/update-repo.sh
  ...

Test 3: Checking git-helpers.sh...
  ✓ git-helpers.sh loads correctly

==============================
All quick tests passed! ✓
==============================
```

### Full Test Success

```
==========================================
  Git Master Skill - Test Suite
==========================================

[INFO] Test repository: git@github.com:dorgonman/kano-git-master-skill-demo.git
[INFO] Test directory: /tmp/tmp.XXXXXXXXXX

[INFO] Test: clone-with-upstream.sh
[PASS] clone-with-upstream.sh: Basic clone

[INFO] Test: update-repo.sh
[PASS] update-repo.sh: Dry-run mode
[PASS] update-repo.sh: Update

[INFO] Test: compare-branches.sh
[PASS] compare-branches.sh: Table format
[PASS] compare-branches.sh: JSON format
[PASS] compare-branches.sh: Markdown output
[PASS] compare-branches.sh: Bidirectional

...

==========================================
  Test Summary
==========================================
Passed: 25
Failed: 0
```

## Troubleshooting

### Test Failures

1. **Script not found**
   - Check file paths
   - Verify folder structure
   - Run from correct directory

2. **Permission denied**
   ```bash
   chmod +x scripts/test/*.sh
   chmod +x scripts/**/*.sh
   ```

3. **SSH authentication failed**
   ```bash
   # Use HTTPS instead
   bash scripts/test/run-all-tests.sh \
     --test-repo https://github.com/dorgonman/kano-git-master-skill-demo.git
   ```

4. **Git command failed**
   ```bash
   # Check git version (need 2.x+)
   git --version
   ```

5. **Test directory not cleaned**
   ```bash
   # Clean manually
   rm -rf /tmp/tmp.*
   ```

### Common Issues

**Issue**: Tests pass locally but fail in CI

**Solution**: 
- Check CI environment has Git 2.x+
- Verify SSH keys or use HTTPS
- Ensure bash 4.0+ available

**Issue**: Cherry-pick tests fail

**Solution**:
- Verify test repository has commits
- Check branch structure
- Ensure no conflicts

**Issue**: Smart commit tests fail

**Solution**:
- Expected if Copilot CLI not installed
- Tests only validate help output
- Full testing requires Copilot

## Adding New Tests

### 1. Add to quick-test.sh

For simple validation:

```bash
# Add to SCRIPTS array
SCRIPTS=(
  # ... existing scripts ...
  "new-category/new-script.sh"
)
```

### 2. Add to run-all-tests.sh

For comprehensive testing:

```bash
test_new_feature() {
  log_info "Test: new-script.sh"
  
  # Setup
  local test_dir="$TEST_DIR/new-test"
  mkdir -p "$test_dir"
  
  # Test
  if bash "$SKILL_ROOT/scripts/new-category/new-script.sh" \
    --option value >/dev/null 2>&1; then
    log_success "new-script.sh: Test passed"
  else
    log_error "new-script.sh: Test failed"
    return 1
  fi
  
  return 0
}

# Add to main()
main() {
  # ... existing tests ...
  test_new_feature || true
}
```

### 3. Add to MANUAL-TESTING.md

Add step-by-step instructions for manual verification.

## Best Practices

1. **Run quick test before every commit**
   ```bash
   bash scripts/test/quick-test.sh
   ```

2. **Run full test before push**
   ```bash
   bash scripts/test/run-all-tests.sh --cleanup
   ```

3. **Use verbose mode for debugging**
   ```bash
   bash scripts/test/run-all-tests.sh --verbose
   ```

4. **Keep test directory for inspection**
   ```bash
   bash scripts/test/run-all-tests.sh
   # Inspect: ls -la /tmp/tmp.XXXXXXXXXX
   ```

5. **Test with different repositories**
   ```bash
   bash scripts/test/run-all-tests.sh \
     --test-repo <your-test-repo>
   ```

## Test Maintenance

### Regular Tasks

- Update test repository if structure changes
- Add tests for new scripts
- Update expected outputs if behavior changes
- Review and update manual testing guide

### Before Release

1. Run full test suite
2. Verify all tests pass
3. Check test coverage
4. Update documentation if needed

## Support

For testing issues:
- Check `scripts/test/README.md` for detailed documentation
- Review `scripts/test/MANUAL-TESTING.md` for step-by-step guide
- Run with `--verbose` flag for detailed output
- Check script help: `./scripts/<script>.sh --help`

## Summary

The Git Master Skill test suite provides:
- ✓ Quick validation (30 seconds)
- ✓ Comprehensive testing (5-10 minutes)
- ✓ Manual testing guide
- ✓ CI/CD integration examples
- ✓ 25+ test cases covering all scripts
- ✓ Multiple output formats
- ✓ Isolated test environment
- ✓ Automatic cleanup

Run tests regularly to ensure quality and catch regressions early.
