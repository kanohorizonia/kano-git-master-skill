# Implementation Plan: Repository Initialization Workflow

## Overview

This implementation plan breaks down the repository initialization workflow into discrete coding tasks. The workflow builds upon existing scripts (`init-empty-repo.sh`, `clone-with-upstream.sh`, `add-submodule.sh`) and introduces new components for multi-remote management, orphan branch creation, and workflow orchestration.

The implementation follows a bottom-up approach: first implementing core utilities, then individual scripts, and finally the orchestration layer.

## Tasks

- [x] 1. Enhance git-helpers.sh with new utility functions
  - Add `gith_is_remote_empty()` function to check if remote has any references
  - Add `gith_validate_url_pair()` function to verify SSH and HTTP URLs point to same repository
  - Add `gith_branch_exists()` function to check if branch exists locally or remotely
  - Add `gith_validate_url()` function to validate Git URL format and protocol
  - Add `gith_validate_branch_name()` function to validate branch names against Git rules
  - Add `gith_ssh_available()` function to test SSH connectivity to a given host
  - _Requirements: 1.1, 1.2, 2.5, 5.8, 7.3, 7.4, 7.6, 11.3_

- [x] 1.1 Write property tests for new git-helpers functions
  - **Property 1: Remote Status Detection**
  - **Property 5: URL Pair Validation**
  - **Property 27: URL Format Validation**
  - **Property 28: Branch Name Validation**
  - **Property 30: Git Repository Validation**
  - _Requirements: 1.1, 1.2, 2.5, 7.3, 7.4, 7.6_

- [x] 2. Implement setup-multi-remote.sh script
  - [x] 2.1 Create script structure with argument parsing
    - Parse --origin-ssh, --origin-http, --upstream-ssh, --upstream-http options
    - Parse --dir, --validate, --dry-run, --help options
    - Validate required arguments (at least origin-ssh or origin-http)
    - _Requirements: 2.1, 2.2_

  - [x] 2.2 Implement basic remote configuration mode
    - Create single "origin" remote when only one URL provided
    - Support both SSH and HTTP as the single URL
    - _Requirements: 2.1_

  - [x] 2.3 Implement advanced remote configuration mode
    - Create origin-ssh, origin-http remotes when both URLs provided
    - Create upstream-ssh, upstream-http remotes when upstream URLs provided
    - Validate URL pairs point to same repository (if --validate flag)
    - _Requirements: 2.2, 2.5_

  - [x] 2.4 Implement push_with_fallback helper function
    - Attempt push to SSH remote first
    - Fallback to HTTP remote if SSH fails
    - Report failure details from both attempts if both fail
    - _Requirements: 2.3, 2.4_

  - [x] 2.5 Write property tests for setup-multi-remote.sh
    - **Property 2: Basic Remote Configuration**
    - **Property 3: Advanced Remote Configuration**
    - **Property 4: Push Fallback Strategy**
    - _Requirements: 2.1, 2.2, 2.3, 2.4_

- [x] 3. Implement create-orphan-branch.sh script
  - [x] 3.1 Create script structure with argument parsing
    - Parse --branch (required), --file, --content, --message options
    - Parse --push, --return, --force-overwrite-branch, --dir, --dry-run options
    - Validate required arguments
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5_

  - [x] 3.2 Implement branch existence validation
    - Check if branch exists locally using git show-ref
    - Check if branch exists remotely using git ls-remote
    - Reject creation if branch exists (unless force flag)
    - _Requirements: 4.1, 7.1_

  - [x] 3.3 Implement stash and branch creation logic
    - Stash uncommitted changes using gith_stash_create
    - Create orphan branch using git checkout --orphan
    - Remove all files from index using git rm -rf
    - Create initial file with specified content
    - Create initial commit
    - _Requirements: 4.2, 4.3, 4.4_

  - [x] 3.4 Implement optional push and return logic
    - Push to remote if --push flag specified
    - Return to original branch if --return flag specified
    - Restore stash using gith_stash_pop when returning
    - _Requirements: 4.5_

  - [x] 3.5 Implement force flag warning mechanism
    - Display warning message when --force-overwrite-branch used
    - Wait 3 seconds before proceeding
    - _Requirements: 7.1, 7.2_

  - [x] 3.6 Write property tests for create-orphan-branch.sh
    - **Property 10: Orphan Branch Name Validation**
    - **Property 11: Orphan Branch Round Trip**
    - **Property 12: Orphan Branch Isolation**
    - **Property 13: Orphan Branch Establishment**
    - **Property 25: Destructive Operation Protection**
    - **Property 26: Force Flag Warning**
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 7.1, 7.2_

- [~] 4. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Enhance add-submodule.sh and create kog-submodule commands
  - [x] 5.1 Add multi-URL support to add-submodule.sh
    - Add --ssh-url and --https-url options
    - Write kog-* extension fields to .gitmodules using git config -f
    - Maintain backward compatibility with single --url option
    - _Requirements: 5.4, 11.1_

  - [x] 5.2 Implement kog-submodule add command
    - Parse --ssh, --https, --path options
    - Call git submodule add with HTTPS URL (default)
    - Add kog-url-ssh, kog-url-https, kog-fetch-priority, kog-push-priority to .gitmodules
    - Configure local .git/config based on SSH availability
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 11.1_

  - [x] 5.3 Implement kog-submodule sync command
    - Read all submodules from .gitmodules
    - For each submodule, read kog-* extension fields
    - Test SSH availability using gith_ssh_available
    - Configure local .git/config with appropriate URL based on priority and availability
    - _Requirements: 5.8, 11.3_

  - [x] 5.4 Implement kog-submodule update command
    - Read kog-url-ssh and kog-url-https from .gitmodules
    - Attempt update with priority URL (SSH)
    - If SSH fails, fallback to HTTPS URL
    - Report which URL was used successfully
    - _Requirements: 5.9, 11.1_

  - [x] 5.5 Add branch and URL validation
    - Check current branch before adding submodule
    - Validate submodule URL is accessible using git ls-remote
    - Report error with diagnostic info if validation fails
    - _Requirements: 5.1, 5.2_

  - [x] 5.6 Add path conflict detection
    - Check if path already contains submodule or directory
    - Report error and skip if conflict exists
    - _Requirements: 5.6, 7.5_

  - [x] 5.7 Write property tests for kog-submodule commands
    - **Property 14: Submodule Branch Validation**
    - **Property 15: Submodule URL Accessibility**
    - **Property 16: Submodule Clone Correctness**
    - **Property 17: Gitmodules Configuration**
    - **Property 19: Submodule Conflict Detection**
    - **Property 20: Gitmodules Extension Field Preservation**
    - **Property 21: Submodule Multi-URL Sync**
    - **Property 22: Submodule Fallback Behavior**
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.6, 5.7, 5.8, 5.9, 11.1, 11.2, 11.3_

- [x] 6. Implement init-repo-workflow.sh orchestration script
  - [x] 6.1 Create script structure with argument parsing
    - Parse --repo-url (required), --repo-http-url, --upstream-ssh, --upstream-http
    - Parse --repo-dir, --orphan-branch, --submodule (repeatable)
    - Parse --skip-main-init, --skip-orphan, --skip-submodules, --dry-run
    - Validate required arguments
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

  - [x] 6.2 Implement remote detection step
    - Call gith_is_remote_empty to check remote status
    - Handle three cases: empty, has-content, not-accessible
    - Log detection result
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 6.1_

  - [x] 6.3 Implement main branch initialization step
    - Skip if --skip-main-init flag set
    - If remote empty: call init-empty-repo.sh
    - If remote has content: clone and pull existing content
    - Log skip reason if skipped due to existing state
    - _Requirements: 3.1, 3.2, 3.3, 6.1, 6.3_

  - [x] 6.4 Implement multi-remote setup step
    - Skip if only one URL provided (basic mode)
    - Call setup-multi-remote.sh if HTTP URL provided
    - Pass all remote URLs to setup script
    - _Requirements: 2.1, 2.2, 6.1_

  - [x] 6.5 Implement orphan branch creation step
    - Skip if --skip-orphan flag set
    - Call create-orphan-branch.sh with specified branch name
    - Log skip reason if skipped
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 6.1, 6.3_

  - [x] 6.6 Implement submodule addition step
    - Skip if --skip-submodules flag set or no submodules specified
    - Process each submodule sequentially
    - Call kog-submodule add for each submodule with SSH and HTTPS URLs
    - Report progress after each submodule
    - Continue on error (don't fail entire workflow)
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.8, 5.9, 6.1, 11.1_

  - [x] 6.7 Implement error handling and rollback
    - Stop execution on step failure
    - Report failure with descriptive error message
    - Implement rollback logic for partial failures
    - _Requirements: 6.2, 8.1, 8.2_

  - [x] 6.8 Implement summary report generation
    - Collect information about all executed steps
    - List created/modified resources
    - List skipped steps with reasons
    - Display summary at end of workflow
    - _Requirements: 6.5_

  - [x] 6.9 Write property tests for init-repo-workflow.sh
    - **Property 29: Workflow Step Ordering**
    - **Property 30: Workflow Failure Propagation**
    - **Property 31: Workflow Step Skipping**
    - **Property 32: Dry Run Idempotence**
    - **Property 33: Workflow Summary Completeness**
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [~] 7. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 8. Implement comprehensive error handling
  - [x] 8.1 Add error classification logic
    - Classify errors as validation, state, network, or git errors
    - Assign appropriate exit codes (1-4)
    - _Requirements: 8.4_

  - [x] 8.2 Add error message formatting
    - Include descriptive error message
    - Include specific recovery steps
    - Include current state information for state errors
    - _Requirements: 8.1, 8.2_

  - [x] 8.3 Add specialized error handlers
    - Implement stash failure handler with recovery commands
    - Implement remote failure handler with error classification
    - _Requirements: 8.3, 8.4_

  - [x] 8.4 Add operation logging
    - Log all operations with timestamps
    - Log operation results (success/failure)
    - Use gith_log function from git-helpers.sh
    - _Requirements: 8.5_

  - [x] 8.5 Write property tests for error handling
    - **Property 40: Error Message Completeness**
    - **Property 41: Stash Recovery Instructions**
    - **Property 42: Remote Error Classification**
    - **Property 43: Operation Logging**
    - _Requirements: 8.1, 8.2, 8.3, 8.4, 8.5_

- [x] 9. Implement input validation across all scripts
  - [x] 9.1 Add URL validation
    - Validate Git URL format (SSH, HTTPS, local path)
    - Reject invalid URLs with clear error message
    - _Requirements: 7.3_

  - [x] 9.2 Add branch name validation
    - Validate against Git branch naming rules
    - Reject invalid characters (spaces, .., ~, ^, :, ?, *, [)
    - _Requirements: 7.4_

  - [x] 9.3 Add path validation
    - Check for conflicts with existing files/directories
    - Reject conflicting paths
    - _Requirements: 7.5_

  - [x] 9.4 Add Git repository validation
    - Check if current directory is in a Git repository
    - Reject operations when not in Git repo
    - _Requirements: 7.6_

  - [x] 9.5 Write property tests for input validation
    - **Property 36: URL Format Validation**
    - **Property 37: Branch Name Validation**
    - **Property 38: Path Conflict Detection**
    - **Property 39: Git Repository Validation**
    - _Requirements: 7.3, 7.4, 7.5, 7.6_

- [x] 10. Implement prerequisite validation for independent script usage
  - [x] 10.1 Add prerequisite checks to each script
    - Check Git is installed
    - Check required arguments are provided
    - Check current directory is Git repo (where applicable)
    - Report missing prerequisites with clear error messages
    - _Requirements: 9.4_

  - [x] 10.2 Write property tests for prerequisite validation
    - **Property 44: Independent Script Prerequisites**
    - _Requirements: 9.4_

- [x] 11. Create integration tests
  - [x] 11.1 Write integration test for complete workflow
    - Test end-to-end workflow with all steps
    - Test workflow with various skip flags
    - Test workflow with dry-run mode
    - Test workflow failure and rollback scenarios
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

  - [x] 11.2 Write integration test for multi-remote with orphan branch
    - Test creating orphan branch with multi-remote setup
    - Test adding submodules to orphan branch with kog-submodule commands
    - Test kog-submodule sync and update with SSH/HTTPS fallback
    - Verify kog-* fields are preserved in .gitmodules after Git operations
    - Test root repo multi-remote configuration with kog-root-remote-* fields
    - _Requirements: 2.1, 2.2, 2.3, 4.1, 4.2, 4.3, 5.1, 5.2, 5.3, 5.8, 5.9, 10.1, 10.2, 10.3, 11.1, 11.2, 11.3_

- [x] 12. Create documentation and examples
  - [x] 12.1 Write usage examples for each script
    - Create examples directory with sample invocations
    - Document common use cases
    - Document error scenarios and recovery
    - _Requirements: All_

  - [x] 12.2 Update main README with workflow documentation
    - Add section describing the workflow
    - Add links to individual script documentation
    - Add troubleshooting guide
    - _Requirements: All_

- [~] 13. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties (minimum 100 iterations each)
- Unit tests validate specific examples and edge cases
- Integration tests validate end-to-end workflows
- All scripts should use shared helper functions from git-helpers.sh to avoid code duplication
- All scripts should follow the existing code style and conventions in the codebase
- All scripts should include comprehensive error handling with descriptive messages
- All scripts should support dry-run mode for safe testing

## Gitmodules Extension Design

The implementation uses `.gitmodules` extension fields with two prefixes for multi-remote support:

### Root Repo Extension Fields (kog-root-remote-*)

**Advantages:**
- Single source of truth (no need to sync multiple files)
- Git-compatible (Git preserves unknown fields)
- Version-controlled and shared across all machines
- Backward compatible (works with standard Git commands)
- Supports multiple remotes with user-defined names

**Extension Fields:**
- `kog-root-remote-<name>-ssh`: SSH URL for remote `<name>` (user-defined)
- `kog-root-remote-<name>-https`: HTTPS URL for remote `<name>` (user-defined)
- `kog-root-push-remote`: Target remote for push operations (default: origin)
- `kog-root-protocol-priority`: Protocol selection (auto|ssh|https, default: auto)

**Protocol Priority:**
- `auto` (default): Auto-detect SSH, prefer SSH, fallback to HTTPS
- `ssh`: Force SSH with HTTPS fallback
- `https`: Force HTTPS only

**Remote Naming:**
- Remote names are completely user-defined
- No hardcoded assumptions (origin, upstream, myfork, gitlab, etc. all supported)
- Supports arbitrary number of remotes

**Commands:**
- `kog-submodule add`: Configure root repo with multi-remote support
- `kog-submodule sync`: Sync root repo remotes with auto protocol selection
- `kog-submodule fetch`: Fetch from all root repo remotes with protocol fallback
- `kog-submodule push`: Push to configured root repo remote with protocol fallback

### Submodule Extension Fields (kog-remote-*)

**Advantages:**
- Single source of truth (no need to sync multiple files)
- Git-compatible (Git preserves unknown fields)
- Version-controlled and shared across all machines
- Backward compatible (works with standard Git commands)
- Supports multiple remotes with user-defined names

**Extension Fields:**
- `kog-remote-<name>-ssh`: SSH URL for remote `<name>` (user-defined)
- `kog-remote-<name>-https`: HTTPS URL for remote `<name>` (user-defined)
- `kog-push-remote`: Target remote for push operations (default: origin)
- `kog-protocol-priority`: Protocol selection (auto|ssh|https, default: auto)

**Protocol Priority:**
- `auto` (default): Auto-detect SSH, prefer SSH, fallback to HTTPS
- `ssh`: Force SSH with HTTPS fallback
- `https`: Force HTTPS only

**Remote Naming:**
- Remote names are completely user-defined
- No hardcoded assumptions (origin, upstream, myfork, gitlab, etc. all supported)
- Supports arbitrary number of remotes

**Commands:**
- `kog-submodule add`: Add submodule with multi-remote support
- `kog-submodule sync`: Sync all remotes with auto protocol selection
- `kog-submodule fetch`: Fetch from all remotes with protocol fallback
- `kog-submodule push`: Push to configured remote with protocol fallback

**Fork Workflow Support:**
```bash
# Fetch from all remotes
git fetch --all

# Rebase onto upstream
git rebase upstream/main

# Push to origin
git push -f origin feature-branch
```

## Recursive Repository Configuration Design

### Problem Statement

A repository can be both:
1. **Root repo** - Has its own submodules and needs to configure its own remotes
2. **Submodule** - Is referenced by another repo (super repo) and needs to configure its submodule remotes

### Solution: Separated Configuration

| Configuration Type | Prefix | Location | Purpose |
|-------------------|--------|----------|---------|
| Root repo remotes | `kog-root-remote-*` | Root repo's `.gitmodules` | Configure root repo's own remotes |
| Submodule remotes | `kog-remote-*` | Super repo's `.gitmodules` | Configure submodule's remotes |

### Example

Root repo `.gitmodules` (configures root repo's own remotes):
```ini
[kog-root-remote "origin"]
    kog-url-ssh = git@github.com:myuser/myproject.git
    kog-url-https = https://github.com/myuser/myproject.git
```

Super repo `.gitmodules` (configures this repo as a submodule):
```ini
[submodule "myproject"]
    path = projects/myproject
    url = https://github.com/myuser/myproject.git

    kog-remote-origin-ssh = git@github.com:myuser/myproject.git
    kog-remote-origin-https = https://github.com/myuser/myproject.git
```

### Commands

- `kog-submodule add` with `--path`: Configures submodule remotes (uses `kog-remote-*`)
- `kog-submodule add` without `--path`: Configures root repo remotes (uses `kog-root-remote-*`)

### Benefits

1. **Clear separation**: Root repo and submodule configurations are completely independent
2. **Single source of truth**: Each repo's configuration is stored in its own `.gitmodules`
3. **Git compatibility**: Both use standard `.gitmodules`, Git preserves unknown fields
4. **No filesystem dependency**: No need for additional config files or directory traversal
5. **Predictable behavior**: Configuration location is always `.gitmodules` in the relevant repo
