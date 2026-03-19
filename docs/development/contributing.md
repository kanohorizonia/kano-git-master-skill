# Contributing Guide

Thank you for your interest in contributing to Git Master Skill!

## Getting Started

### Prerequisites

- Git 2.x or higher
- Bash 4.x or higher (or Git Bash on Windows)
- pixi (recommended for repo-local tool provisioning)
- Basic understanding of shell scripting

### Optional Dependencies

- Python 3.x (for git-p4 features)
- Git Scalar (for mono-repo features)
- git-p4 (for Perforce integration)
- git-svn (for Subversion integration)

## Development Setup

### Clone the Repository

```bash
git clone https://github.com/user/git-master-skill.git
cd git-master-skill
```

### Run Tests

```bash
pixi install
pixi run quick-test
pixi run full-test

# Run specific test directly when iterating on one script
./scripts/test/test-worktree-scripts.sh --test create
```

`pixi` is the repo-level environment/task layer. Native C++ libraries still stay in `src/cpp/vcpkg.json`; see `./pixi.md`.

## Coding Standards

### Script Conventions

All scripts must follow these conventions:

#### 1. Shebang and Strict Mode

```bash
#!/usr/bin/env bash
set -euo pipefail
```

#### 2. Usage Function

```bash
usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS] <required-arg>

Description of what the script does.

OPTIONS:
    --option1           Description of option1
    --option2 <value>   Description of option2
    --dry-run           Show what would be done without doing it
    --help              Show this help message

EXAMPLES:
    $(basename "$0") value
    $(basename "$0") --option1 value
    $(basename "$0") --dry-run value

EOF
}
```

#### 3. Parameter Parsing

```bash
# Default values
DRY_RUN=false
OPTION1=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --option1)
            OPTION1="$2"
            shift 2
            ;;
        --help)
            usage
            exit 0
            ;;
        -*)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
        *)
            REQUIRED_ARG="$1"
            shift
            ;;
    esac
done
```

#### 4. Dry-Run Support

```bash
if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY-RUN] Would execute: git command"
else
    git command
fi
```

#### 5. Error Handling

```bash
# Check prerequisites
if ! command -v git &> /dev/null; then
    echo "Error: git is not installed" >&2
    exit 1
fi

# Validate arguments
if [[ -z "${REQUIRED_ARG:-}" ]]; then
    echo "Error: Missing required argument" >&2
    usage
    exit 1
fi

# Check Git repository
if ! git rev-parse --git-dir &> /dev/null; then
    echo "Error: Not in a Git repository" >&2
    exit 1
fi
```

### Code Style

#### Naming Conventions

- **Variables**: `UPPER_CASE` for constants, `lower_case` for local variables
- **Functions**: `snake_case`
- **Files**: `kebab-case.sh`

#### Comments

```bash
# Single-line comment for simple explanations

# Multi-line comment for complex logic:
# - Point 1
# - Point 2
# - Point 3
```

#### Quoting

```bash
# Always quote variables
echo "$VARIABLE"

# Quote command substitutions
RESULT="$(git command)"

# Use arrays for multiple values
FILES=("file1.txt" "file2.txt")
```

## Adding New Features

### 1. Plan the Feature

- Create an issue describing the feature
- Discuss the approach
- Get feedback from maintainers

### 2. Create the Script

```bash
# Create script file
touch scripts/category/new-feature.sh
chmod +x scripts/category/new-feature.sh

# Add shebang and strict mode
echo '#!/usr/bin/env bash' > scripts/category/new-feature.sh
echo 'set -euo pipefail' >> scripts/category/new-feature.sh
```

### 3. Implement the Feature

- Follow coding standards
- Add usage function
- Support --dry-run
- Add error handling
- Test thoroughly

### 4. Add Tests

```bash
# Create test file
touch scripts/test/test-new-feature.sh
chmod +x scripts/test/test-new-feature.sh

# Implement tests
# See existing test files for examples
```

### 5. Update Documentation

- Add to relevant guide in `docs/guides/`
- Update `docs/README.md` if needed
- Add examples to guide

### 6. Submit Pull Request

- Create a branch: `git checkout -b feature/new-feature`
- Commit changes: `git commit -m "Add new feature"`
- Push branch: `git push origin feature/new-feature`
- Create pull request on GitHub

## Testing

### Manual Testing

```bash
# Test with dry-run first
./scripts/category/new-feature.sh --dry-run

# Test with actual execution
./scripts/category/new-feature.sh

# Test error cases
./scripts/category/new-feature.sh invalid-input
```

### Automated Testing

```bash
# Run test suite
./scripts/test/test-new-feature.sh

# Check exit codes
echo $?  # Should be 0 for success
```

### Cross-Platform Testing

Test on multiple platforms:
- Linux (Ubuntu, Debian, etc.)
- macOS
- Windows (Git Bash)

## Documentation

### Writing Documentation

#### User Guides

Create guides in `docs/guides/`:

```markdown
# Feature Name Guide

**Feature**: Brief description
**Phase**: X
**Status**: Complete

## Overview

Detailed description of the feature.

## Scripts

List of scripts for this feature.

## Features

Key features and capabilities.

## Usage Examples

Practical examples with code blocks.

## Use Cases

When and why to use this feature.

## Requirements

System and software requirements.

## Best Practices

Recommended practices.

## Common Workflows

Step-by-step workflows.

## Troubleshooting

Common issues and solutions.

## See Also

Links to related documentation.
```

#### Comparison Guides

Create comparisons in `docs/comparisons/`:

```markdown
# Feature A vs Feature B

## Overview

Brief comparison summary.

## Comparison Table

| Aspect | Feature A | Feature B |
|--------|-----------|-----------|
| ...    | ...       | ...       |

## When to Use Feature A

Scenarios and benefits.

## When to Use Feature B

Scenarios and benefits.

## Decision Guide

Flowchart or decision tree.
```

### Documentation Style

- Use clear, concise language
- Include code examples
- Add practical use cases
- Provide troubleshooting tips
- Link to related documentation

## Pull Request Process

### Before Submitting

1. **Test thoroughly**: Run `pixi run quick-test` and the relevant deeper test flow
2. **Update documentation**: Add/update relevant docs
3. **Follow conventions**: Ensure code follows standards
4. **Check for errors**: No syntax errors or warnings

### Pull Request Template

```markdown
## Description

Brief description of changes.

## Type of Change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation update
- [ ] Performance improvement

## Testing

- [ ] Tested on Linux
- [ ] Tested on macOS
- [ ] Tested on Windows (Git Bash)
- [ ] Added/updated tests
- [ ] All tests passing

## Documentation

- [ ] Updated relevant guides
- [ ] Added usage examples
- [ ] Updated README if needed

## Checklist

- [ ] Code follows conventions
- [ ] Includes --dry-run support
- [ ] Includes --help option
- [ ] Error handling implemented
- [ ] No breaking changes (or documented)
```

### Review Process

1. **Automated checks**: CI/CD runs tests
2. **Code review**: Maintainers review code
3. **Feedback**: Address review comments
4. **Approval**: Get approval from maintainers
5. **Merge**: Maintainer merges PR

## Release Process

See [Release Process](./release-process.md) for details on how releases are created.

## Getting Help

### Questions

- Open an issue on GitHub
- Tag with `question` label
- Provide context and examples

### Bug Reports

- Open an issue on GitHub
- Tag with `bug` label
- Include:
  - Steps to reproduce
  - Expected behavior
  - Actual behavior
  - Environment details

### Feature Requests

- Open an issue on GitHub
- Tag with `enhancement` label
- Describe:
  - Use case
  - Proposed solution
  - Alternatives considered

## Code of Conduct

### Our Standards

- Be respectful and inclusive
- Welcome newcomers
- Accept constructive criticism
- Focus on what's best for the community

### Unacceptable Behavior

- Harassment or discrimination
- Trolling or insulting comments
- Personal or political attacks
- Publishing others' private information

## License

By contributing, you agree that your contributions will be licensed under the same license as the project.

---

**Thank you for contributing!**

