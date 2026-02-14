# Version Information Extraction Guide

## Overview

The Git Master Skill provides unified version information extraction for:
- **Standard Git** repositories
- **git-p4** repositories (Git synced from Perforce)
- **git-svn** repositories (Git synced from Subversion)

This is essential for CI/CD pipelines, build systems, and version stamping.

## Quick Start

### Basic Usage

```bash
# Get version info for current directory
./scripts/core/get-version-info.sh

# Output:
# Version Information:
# ===================
# vcs_type            : git
# hash_short          : abc123f
# hash_full           : abc123f456789...
# branch              : main
# revision            : 42
# tag                 : v1.0.0
# dirty               : false
```

### Export as Environment Variables

```bash
# Export version variables
eval "$(./scripts/core/get-version-info.sh --export)"

# Use in your scripts
echo "Building version: $PROJECT_REVISION_HASH_SHORT"
echo "Branch: $PROJECT_BRANCH"
echo "Revision: $PROJECT_REVISION"
```

### JSON Output

```bash
# Get JSON output
./scripts/core/get-version-info.sh --format json

# Output:
# {
#   "vcs_type": "git",
#   "hash_short": "abc123f",
#   "hash_full": "abc123f456789...",
#   "branch": "main",
#   "revision": "42",
#   "tag": "v1.0.0",
#   "dirty": false
# }
```

## VCS Type Detection

The tool automatically detects the VCS type:

```bash
# Detect VCS type
./scripts/core/get-version-info.sh --detect-only

# Output: git, git-p4, or git-svn
```

## Revision Offset

### Overview

The revision offset feature allows you to adjust the revision number by adding an offset value. This is particularly useful for large Perforce repositories where you want to start from a smaller number for marketplace publishing.

### Use Case

If your Perforce repository has been running for a long time, the revision count might be very large (e.g., 500300). When publishing to a marketplace, you might want to start from a smaller, more manageable number (e.g., 300).

### How It Works

The offset is added to the Git revision count:

```
PROJECT_REVISION = (Git commit count) + (offset)
```

For example:
- Git commit count: 500300
- Offset: -500000
- Result: 300

### Usage

```bash
# Export with negative offset
eval "$(./scripts/core/get-version-info.sh --export --offset -500000)"

# Check the result
echo $PROJECT_REVISION          # Shows: 300 (instead of 500300)
echo $PROJECT_REVISION_OFFSET   # Shows: -500000

# Use in build scripts
VERSION="${PROJECT_P4_STREAM}-${PROJECT_REVISION}"
echo "Publishing version: $VERSION"
# Output: Publishing version: main-300
```

### Examples

#### Marketplace Publishing (P4)

```bash
#!/usr/bin/env bash
# publish-to-marketplace.sh

# Large P4 repo with 500300 commits
# We want to publish as version 300, not 500300
eval "$(./scripts/core/get-version-info.sh --export --offset -500000)"

# Build version string
MARKETPLACE_VERSION="${PROJECT_P4_PROJECT}-${PROJECT_REVISION}"
echo "Publishing to marketplace: $MARKETPLACE_VERSION"
# Output: Publishing to marketplace: MyProject-300

# Upload to marketplace
upload-to-marketplace --version "$MARKETPLACE_VERSION" --build ./dist/
```

#### CI/CD with Offset

```yaml
# .github/workflows/publish.yml
- name: Get version with offset
  id: version
  run: |
    eval "$(./scripts/core/get-version-info.sh --export --offset -500000)"
    echo "version=$PROJECT_REVISION" >> $GITHUB_OUTPUT
    echo "full_version=$PROJECT_P4_PROJECT-$PROJECT_REVISION" >> $GITHUB_OUTPUT

- name: Publish
  run: |
    echo "Publishing version ${{ steps.version.outputs.full_version }}"
    make publish VERSION=${{ steps.version.outputs.version }}
```

#### Environment Variable

You can also set the offset via environment variable in your build scripts:

```bash
# Set offset for all version queries
export PROJECT_REVISION_OFFSET=-500000

# Your build script can read this
eval "$(./scripts/core/get-version-info.sh --export --offset $PROJECT_REVISION_OFFSET)"
```

### Offset Behavior

- **Default**: 0 (no offset applied)
- **Positive offset**: Increases revision number
- **Negative offset**: Decreases revision number (common use case)
- **Applies to all VCS types**: git, git-p4, git-svn

### Important Notes

1. The offset only affects `PROJECT_REVISION`, not the Git hash or P4 change number
2. The original offset value is stored in `PROJECT_REVISION_OFFSET` for reference
3. Offset is applied consistently across all output formats (text, JSON, env)
4. Use negative offsets to reduce large revision numbers

## Standard Git

### Variables Exported

```bash
PROJECT_REVISION_HASH_SHORT   # Short commit hash (e.g., abc123f)
PROJECT_REVISION_HASH         # Full commit hash
PROJECT_BRANCH                # Current branch name
PROJECT_REVISION              # Commit count (first-parent, with offset applied)
PROJECT_REVISION_OFFSET       # Offset value used (default: 0)
PROJECT_TAG                   # Latest tag
PROJECT_VCS_TYPE              # "git"
```

### Example

```bash
eval "$(./scripts/core/get-version-info.sh --export)"

echo "Version: $PROJECT_TAG-$PROJECT_REVISION-$PROJECT_REVISION_HASH_SHORT"
# Output: Version: v1.0.0-42-abc123f
```

## Git-P4 (Perforce)

### Metadata Format

Git-p4 stores Perforce metadata in commit messages:

```
[git-p4: depot-paths = "//DepotName/StreamName/Project/": change = 12345]
```

### Variables Exported

```bash
PROJECT_REVISION_HASH_SHORT   # Git short hash
PROJECT_REVISION_HASH         # Git full hash
PROJECT_BRANCH                # Git branch name
PROJECT_REVISION              # Git commit count (with offset applied)
PROJECT_REVISION_OFFSET       # Offset value used (default: 0)
PROJECT_DEPOT                 # Perforce depot name
PROJECT_P4_STREAM             # Perforce stream name
PROJECT_P4_PROJECT            # Perforce project name
PROJECT_P4_CHANGE             # Perforce change number
PROJECT_VCS_TYPE              # "git-p4"
```

### Example

```bash
eval "$(./scripts/core/get-version-info.sh --export)"

echo "Depot: $PROJECT_DEPOT"
echo "Stream: $PROJECT_P4_STREAM"
echo "Project: $PROJECT_P4_PROJECT"
echo "P4 Change: $PROJECT_P4_CHANGE"
echo "Git Hash: $PROJECT_REVISION_HASH_SHORT"

# Output:
# Depot: MyDepot
# Stream: main
# Project: MyProject
# P4 Change: 12345
# Git Hash: abc123f
```

### Parsing Details

From metadata: `[git-p4: depot-paths = "//MyDepot/main/MyProject/": change = 12345]`

Extracted values:
- `PROJECT_DEPOT` = `MyDepot` (3rd component of path)
- `PROJECT_P4_STREAM` = `main` (4th component of path)
- `PROJECT_P4_PROJECT` = `MyProject` (5th component of path)
- `PROJECT_P4_CHANGE` = `12345` (change number)

## Git-SVN (Subversion)

### Metadata Format

Git-svn stores SVN metadata in commit messages:

```
git-svn-id: https://svn.example.com/repo/trunk@12345 uuid-here
```

### Variables Exported

```bash
PROJECT_REVISION_HASH_SHORT   # Git short hash
PROJECT_REVISION_HASH         # Git full hash
PROJECT_BRANCH                # Git branch name
PROJECT_REVISION              # Git commit count (with offset applied)
PROJECT_REVISION_OFFSET       # Offset value used (default: 0)
PROJECT_SVN_URL               # SVN repository URL
PROJECT_SVN_REVISION          # SVN revision number
PROJECT_SVN_BRANCH            # SVN branch name (trunk/branch/tag)
PROJECT_VCS_TYPE              # "git-svn"
```

### Example

```bash
eval "$(./scripts/core/get-version-info.sh --export)"

echo "SVN URL: $PROJECT_SVN_URL"
echo "SVN Revision: $PROJECT_SVN_REVISION"
echo "SVN Branch: $PROJECT_SVN_BRANCH"
echo "Git Hash: $PROJECT_REVISION_HASH_SHORT"

# Output:
# SVN URL: https://svn.example.com/repo/trunk
# SVN Revision: 12345
# SVN Branch: trunk
# Git Hash: abc123f
```

### Branch Detection

The tool automatically detects SVN branch from URL:

- `https://svn.example.com/repo/trunk` → `trunk`
- `https://svn.example.com/repo/branches/feature` → `feature`
- `https://svn.example.com/repo/tags/v1.0` → `v1.0`

## CI/CD Integration

### GitHub Actions

```yaml
- name: Get version info
  id: version
  run: |
    eval "$(./scripts/core/get-version-info.sh --export)"
    echo "hash=$PROJECT_REVISION_HASH_SHORT" >> $GITHUB_OUTPUT
    echo "branch=$PROJECT_BRANCH" >> $GITHUB_OUTPUT
    echo "revision=$PROJECT_REVISION" >> $GITHUB_OUTPUT

- name: Build with version
  run: |
    echo "Building version ${{ steps.version.outputs.hash }}"
    make VERSION=${{ steps.version.outputs.hash }}
```

### GitLab CI

```yaml
variables:
  GIT_DEPTH: 0  # Full history for accurate revision count

build:
  script:
    - eval "$(./scripts/core/get-version-info.sh --export)"
    - echo "Building $PROJECT_REVISION_HASH_SHORT"
    - make VERSION=$PROJECT_REVISION_HASH_SHORT
```

### Jenkins

```groovy
pipeline {
  stages {
    stage('Get Version') {
      steps {
        script {
          def versionInfo = sh(
            script: './scripts/core/get-version-info.sh --format json',
            returnStdout: true
          ).trim()
          def version = readJSON text: versionInfo
          env.BUILD_VERSION = version.hash_short
          env.BUILD_BRANCH = version.branch
        }
      }
    }
    stage('Build') {
      steps {
        sh "make VERSION=${env.BUILD_VERSION}"
      }
    }
  }
}
```

## Build System Integration

### CMake

```cmake
# Get version info
execute_process(
  COMMAND bash -c "eval $(./scripts/core/get-version-info.sh --export) && echo $PROJECT_REVISION_HASH_SHORT"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Use in version header
configure_file(
  ${CMAKE_SOURCE_DIR}/version.h.in
  ${CMAKE_BINARY_DIR}/version.h
)
```

### Makefile

```makefile
# Get version info
VERSION_INFO := $(shell eval "$$(./scripts/core/get-version-info.sh --export)" && echo $$PROJECT_REVISION_HASH_SHORT)
BRANCH := $(shell eval "$$(./scripts/core/get-version-info.sh --export)" && echo $$PROJECT_BRANCH)

# Use in build
build:
	@echo "Building version $(VERSION_INFO) from branch $(BRANCH)"
	gcc -DVERSION=\"$(VERSION_INFO)\" -o myapp main.c
```

### Unreal Engine

```bash
#!/usr/bin/env bash
# ue_build_version.sh

# Get version info
eval "$(./scripts/core/get-version-info.sh --export)"

# Detect VCS type
case "$PROJECT_VCS_TYPE" in
  git-p4)
    # Use P4 change number for UE version
    UE_VERSION="$PROJECT_P4_STREAM-$PROJECT_P4_CHANGE"
    ;;
  git-svn)
    # Use SVN revision for UE version
    UE_VERSION="$PROJECT_SVN_BRANCH-$PROJECT_SVN_REVISION"
    ;;
  git)
    # Use Git tag and revision
    UE_VERSION="$PROJECT_TAG-$PROJECT_REVISION"
    ;;
esac

echo "Building Unreal Engine project version: $UE_VERSION"

# Pass to UE build
RunUAT BuildCookRun \
  -project="MyProject.uproject" \
  -clientconfig=Development \
  -serverconfig=Development \
  -build \
  -cook \
  -stage \
  -pak \
  -archive \
  -archivedirectory="Builds/$UE_VERSION"
```

## Advanced Usage

### Custom Format

```bash
# Get specific fields only
eval "$(./scripts/core/get-version-info.sh --export)"

# Create custom version string
VERSION_STRING="${PROJECT_BRANCH}-${PROJECT_REVISION}-${PROJECT_REVISION_HASH_SHORT}"
echo "Version: $VERSION_STRING"

# For git-p4
if [[ "$PROJECT_VCS_TYPE" == "git-p4" ]]; then
  P4_VERSION="${PROJECT_DEPOT}/${PROJECT_P4_STREAM}@${PROJECT_P4_CHANGE}"
  echo "P4 Version: $P4_VERSION"
fi
```

### Version Stamping

```bash
#!/usr/bin/env bash
# stamp-version.sh - Stamp version into source files

eval "$(./scripts/core/get-version-info.sh --export)"

# Generate version header
cat > include/version.h <<EOF
#ifndef VERSION_H
#define VERSION_H

#define VERSION_HASH "$PROJECT_REVISION_HASH"
#define VERSION_HASH_SHORT "$PROJECT_REVISION_HASH_SHORT"
#define VERSION_BRANCH "$PROJECT_BRANCH"
#define VERSION_REVISION $PROJECT_REVISION
#define VERSION_VCS_TYPE "$PROJECT_VCS_TYPE"

EOF

# Add VCS-specific info
case "$PROJECT_VCS_TYPE" in
  git-p4)
    cat >> include/version.h <<EOF
#define VERSION_P4_DEPOT "$PROJECT_DEPOT"
#define VERSION_P4_STREAM "$PROJECT_P4_STREAM"
#define VERSION_P4_CHANGE $PROJECT_P4_CHANGE

EOF
    ;;
  git-svn)
    cat >> include/version.h <<EOF
#define VERSION_SVN_URL "$PROJECT_SVN_URL"
#define VERSION_SVN_REVISION $PROJECT_SVN_REVISION
#define VERSION_SVN_BRANCH "$PROJECT_SVN_BRANCH"

EOF
    ;;
esac

cat >> include/version.h <<EOF
#endif // VERSION_H
EOF

echo "Version header generated: include/version.h"
```

## Library Usage

### In Shell Scripts

```bash
#!/usr/bin/env bash

# Source the library
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/version-helpers.sh"

# Detect VCS type
vcs_type=$(detect_vcs_type ".")
echo "VCS Type: $vcs_type"

# Get version info as JSON
version_json=$(get_version_info ".")
echo "$version_json"

# Export variables
eval "$(export_version_vars ".")"
echo "Hash: $PROJECT_REVISION_HASH_SHORT"
```

### Functions Available

```bash
# Detect VCS type
detect_vcs_type [repo_path]

# Get version info (auto-detect VCS)
get_version_info [repo_path]

# Get standard Git info
get_git_version_info [repo_path]

# Get git-p4 info
get_p4_version_info [repo_path]

# Get git-svn info
get_svn_version_info [repo_path]

# Export as environment variables
export_version_vars [repo_path]

# Print human-readable format
print_version_info [repo_path]
```

## Troubleshooting

### No git-p4 metadata found

**Problem**: Script reports "Not a git-p4 repository"

**Solution**: Check if latest commit has git-p4 metadata:
```bash
git log -1 --format=%B | grep "git-p4:"
```

If no metadata, the repository might not be synced from Perforce.

### No git-svn-id found

**Problem**: Script reports "Not a git-svn repository"

**Solution**: Check if latest commit has git-svn-id:
```bash
git log -1 --format=%B | grep "git-svn-id:"
```

If no metadata, the repository might not be synced from Subversion.

### Incorrect P4 depot/stream parsing

**Problem**: Depot or stream name is empty or incorrect

**Solution**: Check the depot-paths format in commit message:
```bash
git log -1 --format=%B | grep "depot-paths"
```

Expected format: `depot-paths = "//DepotName/StreamName/Project/"`

### Detached HEAD state

**Problem**: `PROJECT_BRANCH` shows "detached"

**Solution**: This is normal when not on a branch. Use the hash instead:
```bash
if [[ "$PROJECT_BRANCH" == "detached" ]]; then
  VERSION="$PROJECT_REVISION_HASH_SHORT"
else
  VERSION="$PROJECT_BRANCH-$PROJECT_REVISION_HASH_SHORT"
fi
```

## Best Practices

1. **Always use full history**: Set `GIT_DEPTH=0` in CI to get accurate revision counts

2. **Cache version info**: In build scripts, export once and reuse:
   ```bash
   eval "$(./scripts/core/get-version-info.sh --export)"
   # Now use $PROJECT_* variables throughout
   ```

3. **Handle all VCS types**: Check `$PROJECT_VCS_TYPE` and handle accordingly:
   ```bash
   case "$PROJECT_VCS_TYPE" in
     git-p4) echo "P4 Change: $PROJECT_P4_CHANGE" ;;
     git-svn) echo "SVN Rev: $PROJECT_SVN_REVISION" ;;
     git) echo "Git Tag: $PROJECT_TAG" ;;
   esac
   ```

4. **Version stamping**: Generate version headers during build, not in source control

5. **Semantic versioning**: Combine with tags for proper versioning:
   ```bash
   VERSION="$PROJECT_TAG+$PROJECT_REVISION.$PROJECT_REVISION_HASH_SHORT"
   # Example: v1.0.0+42.abc123f
   ```

## Examples

See `examples/version-stamping/` for complete examples:
- CMake integration
- Makefile integration
- Unreal Engine integration
- Docker image tagging
- Semantic versioning

## Related Tools

- `scripts/vcs-bridges/p4/strip-metadata.sh` - Strip git-p4 metadata from commits
- `scripts/core/discover-repos.sh` - Discover repositories with version info
- `scripts/workspace/status-all-repos.sh` - Show version info for all repos

## Summary

The version information extraction tool provides:
- ✅ Unified interface for Git, git-p4, and git-svn
- ✅ Multiple output formats (text, JSON, env)
- ✅ Easy CI/CD integration
- ✅ Build system integration
- ✅ Automatic VCS type detection
- ✅ Comprehensive metadata extraction

Perfect for version stamping, build automation, and release management!
