# Git Master Skill - Test Suite

Comprehensive test suite for validating all Git Master Skill scripts.

📊 **Visual Guide**: See [TEST-DIAGRAMS.md](TEST-DIAGRAMS.md) for Mermaid diagrams explaining test flows and scenarios.

## Quick Start

### Quick Smoke Test

Run quick validation of all scripts (no repository needed):

```bash
cd skills/kano-git-master-skill
bash scripts/test/quick-test.sh
```

This checks:
- All script files exist
- Help commands work
- Helper library loads correctly

### Full Test Suite

Run comprehensive tests with a real repository:

```bash
cd skills/kano-git-master-skill
bash scripts/test/run-all-tests.sh --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git
```

Options:
- `--test-repo <url>` - Specify test repository (default: kano-git-master-skill-demo)
- `--cleanup` - Remove test directory after completion
- `--verbose` - Show detailed output
- `-h, --help` - Show help

## Test Coverage

The full test suite covers:

### Repository Management Scripts
- ✓ `clone-with-upstream.sh` - Clone with upstream remote
- ✓ `update-repo.sh` - Update repository and submodules
- ✓ `discover-repos.sh` - Discover all repositories

### Workspace Scripts
- ✓ `status-all-repos.sh` - Status reporting (table/JSON/markdown)
- ✓ `foreach-repo.sh` - Execute commands across repos
- ✓ `update-workspace-repos.sh` - Batch updates
- ✓ `test-native-planner-contract.sh` - Validate native planner JSON contract for update/foreach

### Branch Operations
- ✓ `compare-branches.sh` - Branch comparison (all formats)
- ✓ `cherry-pick-batch.sh` - Batch cherry-pick (JSON/text)
- ✓ `rebase-to-upstream-latest.sh` - Rebase to upstream

### Commit Tools
- ✓ `smart-commit.sh` - AI-powered commit (help only, requires Copilot)

## Test Repository

The test suite uses: `git@github.com:dorgonman/kano-git-master-skill-demo.git`

### Initialize Test Repository

If the repository is empty, initialize it first:

```bash
# Quick initialization
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git

# With more options
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --commits 10 \
  --with-submodule
```

See [INIT-REPO-GUIDE.md](INIT-REPO-GUIDE.md) for detailed initialization instructions.

### Repository Requirements

This repository should:
- Be publicly accessible or accessible with your SSH key
- Have a `main` branch
- Be a simple repository for testing

## Running Individual Tests

You can run specific test functions by sourcing the test script:

```bash
cd skills/kano-git-master-skill
source scripts/test/run-all-tests.sh

# Run specific test
test_compare_branches
test_cherry_pick_batch

# Validate native planner contract (uses built C++ binary if present)
bash scripts/test/test-native-planner-contract.sh
```

## Test Output

### Quick Test Output
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

### Full Test Output
```
==========================================
  Git Master Skill - Test Suite
==========================================

[INFO] Test repository: git@github.com:dorgonman/kano-git-master-skill-demo.git
[INFO] Test directory: /tmp/tmp.XXXXXXXXXX
[INFO] Skill root: /path/to/skills/kano-git-master-skill

[INFO] Running: clone-with-upstream.sh
[PASS] clone-with-upstream.sh: Basic clone
[PASS] clone-with-upstream.sh: Clone directory created

[INFO] Running: update-repo.sh
[PASS] update-repo.sh: Dry-run mode
[PASS] update-repo.sh: Update

...

==========================================
  Test Summary
==========================================
Passed: 25
Failed: 0
```

## Troubleshooting

### SSH Key Issues

If you get authentication errors:

```bash
# Check SSH key
ssh -T git@github.com

# Or use HTTPS instead
bash scripts/test/run-all-tests.sh \
  --test-repo https://github.com/dorgonman/kano-git-master-skill-demo.git
```

### Permission Denied

Make scripts executable:

```bash
chmod +x scripts/test/*.sh
chmod +x scripts/**/*.sh
```

### Test Directory Not Cleaned

If tests fail and leave test directory:

```bash
# Find test directories
ls -la /tmp/tmp.*

# Clean manually
rm -rf /tmp/tmp.XXXXXXXXXX
```

## CI/CD Integration

### GitHub Actions

```yaml
name: Test Git Master Skill

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Run quick tests
        run: bash skills/kano-git-master-skill/scripts/test/quick-test.sh
      
      - name: Run full tests
        run: |
          bash skills/kano-git-master-skill/scripts/test/run-all-tests.sh \
            --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
            --cleanup
```

### GitLab CI

```yaml
test:
  script:
    - bash skills/kano-git-master-skill/scripts/test/quick-test.sh
    - bash skills/kano-git-master-skill/scripts/test/run-all-tests.sh --cleanup
```

## Adding New Tests

To add a new test function:

1. Add function to `run-all-tests.sh`:

```bash
test_my_new_feature() {
  log_info "Test: my-new-script.sh"
  
  # Setup
  local test_dir="$TEST_DIR/my-test"
  mkdir -p "$test_dir"
  
  # Test
  if bash "$SKILL_ROOT/scripts/my-new-script.sh" >/dev/null 2>&1; then
    log_success "my-new-script.sh: Test passed"
  else
    log_error "my-new-script.sh: Test failed"
    return 1
  fi
  
  return 0
}
```

2. Call it in `main()`:

```bash
main() {
  # ... existing tests ...
  test_my_new_feature || true
}
```

## Notes

- Tests use temporary directories that are cleaned up automatically
- Use `--cleanup` flag to remove test directory after completion
- Some tests (like `smart-commit.sh`) require external tools (GitHub Copilot CLI)
- Tests are designed to be non-destructive and isolated

## Support

For issues or questions:
- Check script help: `./scripts/<script-name>.sh --help`
- Review documentation: `docs/README.md`
- Check examples: `docs/USAGE-EXAMPLES.md`
- View diagrams: `scripts/test/TEST-DIAGRAMS.md` or open `scripts/test/preview-diagrams.html` in browser

## Viewing Test Diagrams

### Option 1: HTML Preview (Easiest)

Open the HTML file in your browser:

```bash
# Windows
start scripts/test/preview-diagrams.html

# macOS
open scripts/test/preview-diagrams.html

# Linux
xdg-open scripts/test/preview-diagrams.html
```

### Option 2: Mermaid Live Editor

1. Open https://mermaid.live/
2. Copy diagram code from `TEST-DIAGRAMS.md`
3. Paste and view

### Option 3: VS Code

Install extension and preview markdown:

```bash
code --install-extension bierner.markdown-mermaid
code scripts/test/TEST-DIAGRAMS.md
```

### Option 4: Generate Images

```bash
# Install mermaid-cli
npm install -g @mermaid-js/mermaid-cli

# Generate diagrams
bash scripts/test/generate-diagrams.sh --format png
```

See `TEST-DIAGRAMS.md` for complete diagram documentation.
