# Test Repository Initialization Guide

Guide for initializing the test repository `git@github.com:dorgonman/kano-git-master-skill-demo.git`.

## Quick Start

### Using the Script (Recommended)

```bash
# Navigate to the skill directory
cd skills/kano-git-master-skill

# Run the initialization script
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git
```

### What the Script Does

1. ✅ Creates a local Git repository
2. ✅ Adds initial project structure:
   - `README.md` - Project description
   - `.gitignore` - Common ignore patterns
   - `src/` - Sample source code
   - `docs/` - Documentation
   - `tests/` - Test scripts
3. ✅ Creates multiple commits (default: 5)
4. ✅ Pushes to remote repository

## Script Options

### Basic Usage

```bash
# Initialize with defaults
bash scripts/test/init-test-repo.sh git@github.com:dorgonman/kano-git-master-skill-demo.git
```

### Custom Directory

```bash
# Use specific directory
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --dir /tmp/test-repo
```

### More Commits

```bash
# Create 10 commits instead of 5
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --commits 10
```

### With Submodule

```bash
# Add a test submodule
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --with-submodule
```

### Force Push

```bash
# Force overwrite (DANGEROUS - use with extreme caution!)
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --force-overwrite-remote
```

**Safety Note**: The script will:
1. Check if remote already has content
2. Show what will be destroyed
3. Wait 3 seconds before proceeding
4. Give you time to press Ctrl+C to cancel

## Manual Initialization

If you prefer to initialize manually:

### Step 1: Create Local Repository

```bash
# Create directory
mkdir -p /tmp/test-repo
cd /tmp/test-repo

# Initialize git
git init
git checkout -b main
```

### Step 2: Create Initial Structure

```bash
# Create README
cat > README.md <<'EOF'
# Test Repository

This is a test repository for Git Master Skill testing.

## Purpose

Testing various Git operations:
- Repository updates
- Branch comparisons
- Cherry-picking
- Submodule handling

## Structure

- `src/` - Source code
- `docs/` - Documentation
- `tests/` - Test scripts
EOF

# Create .gitignore
cat > .gitignore <<'EOF'
.DS_Store
*.swp
*.log
node_modules/
.env
EOF

# Create source directory
mkdir -p src
cat > src/main.sh <<'EOF'
#!/usr/bin/env bash
echo "Hello from test repository!"
EOF
chmod +x src/main.sh

# Create docs
mkdir -p docs
cat > docs/guide.md <<'EOF'
# User Guide

Basic usage guide for test repository.
EOF

# Create tests
mkdir -p tests
cat > tests/test.sh <<'EOF'
#!/usr/bin/env bash
echo "Running tests..."
[[ -f "src/main.sh" ]] && echo "✓ main.sh exists"
EOF
chmod +x tests/test.sh
```

### Step 3: Initial Commit

```bash
git add .
git commit -m "Initial commit: Add project structure"
```

### Step 4: Create Additional Commits

```bash
# Commit 1
echo "# Feature 1" >> README.md
git add README.md
git commit -m "docs: Add feature 1"

# Commit 2
echo "# Feature 2" >> src/main.sh
git add src/main.sh
git commit -m "feat: Add feature 2"

# Commit 3
echo "## Feature 3" >> docs/guide.md
git add docs/guide.md
git commit -m "docs: Document feature 3"

# Commit 4
echo "# Test 4" >> tests/test.sh
git add tests/test.sh
git commit -m "test: Add test 4"

# Commit 5
echo "# Update 5" >> README.md
git add README.md
git commit -m "chore: Update 5"
```

### Step 5: Push to Remote

```bash
# Add remote
git remote add origin git@github.com:dorgonman/kano-git-master-skill-demo.git

# Push
git push -u origin main
```

## Repository Structure

After initialization, the repository will have:

```
test-repo/
├── README.md           # Project description
├── .gitignore          # Ignore patterns
├── src/                # Source code
│   ├── main.sh         # Main script
│   └── utils.sh        # Utility functions
├── docs/               # Documentation
│   └── guide.md        # User guide
└── tests/              # Test scripts
    └── test.sh         # Test script
```

## Commit History

The repository will have commits like:

```
* chore: Update 5
* test: Add test 4
* docs: Document feature 3
* feat: Add feature 2
* docs: Add feature 1
* Initial commit: Add project structure
```

## Verification

After initialization, verify the repository:

```bash
# Clone the repository
git clone git@github.com:dorgonman/kano-git-master-skill-demo.git
cd kano-git-master-skill-demo

# Check structure
ls -la

# Check commits
git log --oneline

# Check branches
git branch -a

# Run test
bash tests/test.sh
```

## Troubleshooting

### SSH Authentication Failed

If you get SSH authentication errors:

```bash
# Check SSH key
ssh -T git@github.com

# Or use HTTPS
bash scripts/test/init-test-repo.sh \
  https://github.com/dorgonman/kano-git-master-skill-demo.git
```

### Permission Denied

Make script executable:

```bash
chmod +x scripts/test/init-test-repo.sh
```

### Repository Already Exists

If the remote repository already has content:

```bash
# Script will detect and refuse to overwrite
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git

# Output will show:
# [✗] Remote repository is NOT empty!
# [✗] Refusing to overwrite existing content
# [✗] If you really want to overwrite, use: --force-overwrite-remote

# Only if you REALLY want to destroy existing content:
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --force-overwrite-remote

# This will:
# 1. Show what will be destroyed
# 2. Display a 3-second warning
# 3. Give you time to press Ctrl+C to cancel
```

**Important**: The flag name `--force-overwrite-remote` is intentionally verbose to prevent accidental data loss. The old `--force` flag is rejected with a helpful error message.

### Git Bash on Windows

On Windows, use Git Bash:

```bash
# Open Git Bash
# Navigate to skill directory
cd /d/_work/_Kano/kano-opencode-quickstart/skills/kano-git-master-skill

# Run script
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git
```

## Using with Tests

After initializing the repository, you can run tests:

```bash
# Quick test
bash scripts/test/quick-test.sh

# Full test suite
bash scripts/test/run-all-tests.sh \
  --test-repo git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --cleanup
```

## Reinitializing

To reinitialize the repository:

```bash
# Delete local copy
rm -rf /tmp/test-repo

# Script will check remote and refuse if not empty
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git

# To force overwrite (DANGEROUS):
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --force-overwrite-remote
```

## Advanced Options

### Custom Branch Name

```bash
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --branch develop
```

### Specific Number of Commits

```bash
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --commits 20
```

### With Submodule

```bash
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --with-submodule
```

### All Options Combined

```bash
bash scripts/test/init-test-repo.sh \
  git@github.com:dorgonman/kano-git-master-skill-demo.git \
  --dir /tmp/my-test-repo \
  --branch main \
  --commits 10 \
  --with-submodule
```

## Notes

- The script uses a temporary directory by default
- Local directory is cleaned up on system reboot if using temp
- Use `--dir` to specify a permanent location
- Always verify SSH access before running
- **Safety first**: Script checks if remote has content before pushing
- Use `--force-overwrite-remote` with extreme caution as it destroys remote content
- The verbose flag name is intentional to prevent accidental overwrites

## Support

For issues:
- Check SSH keys: `ssh -T git@github.com`
- Verify Git installation: `git --version`
- Check script permissions: `ls -la scripts/test/init-test-repo.sh`
- Use `--help` flag: `bash scripts/test/init-test-repo.sh --help`
