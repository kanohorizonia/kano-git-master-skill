# Testing Guide

This guide explains how to test Git Master Skill scripts.

## Test Structure

### Test Files

Tests are located in `scripts/test/`:

```
scripts/test/
├── test-revision-offset.sh      # Version info tests
└── test-worktree-scripts.sh     # Worktree tests
```

### Test Format

Each test file follows this structure:

```bash
#!/usr/bin/env bash
set -euo pipefail

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test function
test_feature() {
    local test_name="$1"
    TESTS_RUN=$((TESTS_RUN + 1))
    
    echo "Running: $test_name"
    
    # Test logic here
    if [[ condition ]]; then
        echo "  ✓ PASSED"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ✗ FAILED"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Run tests
test_feature "Test description"

# Summary
echo ""
echo "Tests run: $TESTS_RUN"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"

# Exit with appropriate code
[[ $TESTS_FAILED -eq 0 ]]
```

## Running Tests

If `pixi` is available, use it as the default repo entrypoint. The tasks in `pixi.toml` call the existing test scripts and keep their behavior intact.

### All Tests

```bash
pixi install
pixi run quick-test
pixi run full-test
```

### Specific Tests

```bash
# Run specific test file
./scripts/test/test-worktree-scripts.sh

# Run specific test within file
./scripts/test/test-worktree-scripts.sh --test create
```

### Verbose Output

```bash
# Run with verbose output
./scripts/test/test-worktree-scripts.sh --verbose
```

## Test Categories

### Unit Tests

Test individual functions in isolation.

**Example**:
```bash
test_parse_version() {
    local version="1.2.3"
    local result
    result=$(parse_version "$version")
    
    if [[ "$result" == "1.2.3" ]]; then
        echo "  ✓ PASSED"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ✗ FAILED: Expected 1.2.3, got $result"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}
```

### Integration Tests

Test scripts end-to-end.

**Example**:
```bash
test_create_worktree() {
    local branch="test-branch"
    
    # Create worktree
    ./scripts/worktree/create-worktree.sh "$branch"
    
    # Verify worktree exists
    if git worktree list | grep -q "$branch"; then
        echo "  ✓ PASSED"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ✗ FAILED: Worktree not created"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    # Cleanup
    git worktree remove "../repo-$branch" --force
}
```

### Error Handling Tests

Test error conditions and edge cases.

**Example**:
```bash
test_invalid_input() {
    local result
    
    # Test with invalid input
    if ./scripts/worktree/create-worktree.sh "" 2>/dev/null; then
        echo "  ✗ FAILED: Should have rejected empty input"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    else
        echo "  ✓ PASSED: Correctly rejected empty input"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    fi
}
```

## Writing Tests

### Test Template

```bash
#!/usr/bin/env bash
set -euo pipefail

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Test function
run_test() {
    local test_name="$1"
    local test_func="$2"
    
    TESTS_RUN=$((TESTS_RUN + 1))
    echo ""
    echo "Test $TESTS_RUN: $test_name"
    
    if $test_func; then
        echo "  ✓ PASSED"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "  ✗ FAILED"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Individual test functions
test_basic_functionality() {
    # Test logic here
    return 0  # or 1 for failure
}

test_error_handling() {
    # Test logic here
    return 0  # or 1 for failure
}

# Run tests
run_test "Basic functionality" test_basic_functionality
run_test "Error handling" test_error_handling

# Summary
echo ""
echo "================================"
echo "Test Summary"
echo "================================"
echo "Tests run: $TESTS_RUN"
echo "Passed: $TESTS_PASSED"
echo "Failed: $TESTS_FAILED"
echo ""

# Exit with appropriate code
[[ $TESTS_FAILED -eq 0 ]]
```

### Best Practices

#### 1. Test Setup and Cleanup

```bash
setup_test() {
    # Create test environment
    TEST_DIR=$(mktemp -d)
    cd "$TEST_DIR"
    git init
}

cleanup_test() {
    # Clean up test environment
    cd /
    rm -rf "$TEST_DIR"
}

# Use in tests
test_feature() {
    setup_test
    
    # Test logic
    
    cleanup_test
}
```

#### 2. Isolate Tests

```bash
# Each test should be independent
test_feature_a() {
    # Setup
    local test_dir=$(mktemp -d)
    
    # Test
    # ...
    
    # Cleanup
    rm -rf "$test_dir"
}

test_feature_b() {
    # Setup
    local test_dir=$(mktemp -d)
    
    # Test
    # ...
    
    # Cleanup
    rm -rf "$test_dir"
}
```

#### 3. Test Both Success and Failure

```bash
test_success_case() {
    # Test successful execution
    if ./script.sh valid-input; then
        return 0
    else
        return 1
    fi
}

test_failure_case() {
    # Test error handling
    if ./script.sh invalid-input 2>/dev/null; then
        return 1  # Should have failed
    else
        return 0  # Correctly failed
    fi
}
```

#### 4. Use Descriptive Names

```bash
# Good
test_create_worktree_with_new_branch()
test_remove_worktree_with_uncommitted_changes()

# Bad
test_1()
test_worktree()
```

#### 5. Test Edge Cases

```bash
test_empty_input()
test_special_characters()
test_very_long_input()
test_concurrent_execution()
```

## Test Coverage

### Current Coverage

| Feature | Tests | Status |
|---------|-------|--------|
| Version Info | 6 | ✅ Complete |
| Worktree | 10 | ✅ Complete |
| Subtree | 0 | 🔲 Needed |
| Submodule | 0 | 🔲 Needed |
| Scalar | 0 | 🔲 Needed |
| Git-P4 | 0 | 🔲 Needed |
| Git-SVN | 0 | 🔲 Needed |

### Adding Test Coverage

To add tests for a feature:

1. Create test file: `scripts/test/test-feature.sh`
2. Implement test cases
3. Run tests to verify
4. Update coverage table above

## Continuous Integration

### GitHub Actions

Create `.github/workflows/test.yml`:

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    
    steps:
      - uses: actions/checkout@v2
      - name: Install pixi
        run: curl -fsSL https://pixi.sh/install.sh | bash
      - name: Install workspace tools
        run: pixi install --locked
      
      - name: Run tests
        run: pixi run full-test
```

### Local CI Simulation

```bash
# Run all tests like CI would
pixi install
pixi run full-test
```

## Debugging Tests

### Verbose Mode

```bash
# Add verbose output to tests
set -x  # Enable command tracing

# Run test
./scripts/test/test-feature.sh

set +x  # Disable command tracing
```

### Debugging Individual Tests

```bash
# Run specific test with debugging
bash -x ./scripts/test/test-feature.sh --test specific-test
```

### Preserving Test Environment

```bash
# Don't cleanup on failure
cleanup_test() {
    if [[ $TESTS_FAILED -eq 0 ]]; then
        rm -rf "$TEST_DIR"
    else
        echo "Test failed, preserving environment at: $TEST_DIR"
    fi
}
```

## Performance Testing

### Timing Tests

```bash
test_performance() {
    local start_time
    local end_time
    local duration
    
    start_time=$(date +%s)
    
    # Run operation
    ./script.sh
    
    end_time=$(date +%s)
    duration=$((end_time - start_time))
    
    echo "Duration: ${duration}s"
    
    # Assert performance requirement
    if [[ $duration -lt 5 ]]; then
        return 0
    else
        echo "Too slow: ${duration}s (expected < 5s)"
        return 1
    fi
}
```

### Benchmarking

```bash
# Run operation multiple times
benchmark_operation() {
    local iterations=10
    local total_time=0
    
    for ((i=1; i<=iterations; i++)); do
        local start=$(date +%s%N)
        ./script.sh
        local end=$(date +%s%N)
        local duration=$(( (end - start) / 1000000 ))
        total_time=$((total_time + duration))
    done
    
    local avg_time=$((total_time / iterations))
    echo "Average time: ${avg_time}ms"
}
```

## Test Reporting

### JUnit XML Format

```bash
# Generate JUnit XML report
generate_junit_report() {
    cat > test-results.xml << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="Git Master Skill Tests" tests="$TESTS_RUN" failures="$TESTS_FAILED">
  <testcase name="Test 1" />
  <testcase name="Test 2" />
</testsuite>
EOF
}
```

### HTML Report

```bash
# Generate HTML report
generate_html_report() {
    cat > test-results.html << EOF
<!DOCTYPE html>
<html>
<head><title>Test Results</title></head>
<body>
  <h1>Test Results</h1>
  <p>Tests run: $TESTS_RUN</p>
  <p>Passed: $TESTS_PASSED</p>
  <p>Failed: $TESTS_FAILED</p>
</body>
</html>
EOF
}
```

## See Also

- [Contributing Guide](./contributing.md)
- [Release Process](./release-process.md)

---

**Last Updated**: 2026-02-12
