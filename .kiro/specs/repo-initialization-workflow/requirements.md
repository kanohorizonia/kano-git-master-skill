# Requirements Document

## Introduction

This document specifies the requirements for a comprehensive repository initialization workflow that supports multi-remote management, orphan branch creation, and submodule integration. The workflow enables developers to initialize new repositories with flexible remote configurations (SSH/HTTP fallback), create isolated orphan branches for development tooling, and manage dependencies through Git submodules.

## Glossary

- **Repository**: A Git repository that can be local or remote
- **Remote**: A named reference to a remote Git repository URL
- **Origin**: The primary remote repository (typically the user's fork or main working repository)
- **Upstream**: The original source repository (used when working with forks)
- **Orphan_Branch**: A Git branch with no parent commits, creating an independent history
- **Submodule**: A Git repository embedded within another Git repository
- **SSH_URL**: A Git remote URL using SSH protocol (e.g., git@github.com:user/repo.git)
- **HTTP_URL**: A Git remote URL using HTTPS protocol (e.g., https://github.com/user/repo.git)
- **Main_Branch**: The primary branch of a repository (e.g., main, master)
- **Remote_Manager**: The system component responsible for managing multiple remote configurations
- **Initialization_Workflow**: The complete process of setting up a repository with remotes, branches, and submodules

## Requirements

### Requirement 1: Remote Repository Detection

**User Story:** As a developer, I want the system to detect whether a remote repository is empty or has existing content, so that I can avoid accidentally overwriting existing work.

#### Acceptance Criteria

1. WHEN a remote URL is provided, THE System SHALL check if the remote repository contains any references
2. WHEN the remote repository has zero references, THE System SHALL classify it as empty
3. WHEN the remote repository has one or more references, THE System SHALL classify it as non-empty and list the existing references
4. IF the remote repository is not accessible, THEN THE System SHALL report a connectivity error with diagnostic information

### Requirement 2: Multi-Remote Configuration Management

**User Story:** As a developer, I want to configure multiple remotes with SSH and HTTP URLs, so that I can have automatic fallback when SSH authentication fails.

#### Acceptance Criteria

1. WHEN only one URL is provided, THE Remote_Manager SHALL create a basic remote configuration with origin only
2. WHEN both SSH and HTTP URLs are provided, THE Remote_Manager SHALL create an advanced configuration with origin-ssh, origin-http, upstream-ssh, and upstream-http remotes
3. WHEN pushing to a remote, THE Remote_Manager SHALL attempt SSH first and fallback to HTTP if SSH fails
4. WHEN a remote operation fails on both SSH and HTTP, THE Remote_Manager SHALL report the failure with details from both attempts
5. THE Remote_Manager SHALL validate that SSH and HTTP URLs point to the same repository before creating advanced configuration

### Requirement 3: Main Branch Initialization

**User Story:** As a developer, I want to initialize the main branch with a simple README file, so that the repository has a valid starting point.

#### Acceptance Criteria

1. WHEN initializing an empty remote repository, THE System SHALL create a main branch with a single README.md file
2. WHEN the remote repository already has content on the main branch, THE System SHALL pull the existing content instead of creating new content
3. WHEN the remote repository has content but on a different default branch, THE System SHALL detect and use that branch name
4. THE System SHALL allow customization of the initial commit message and README content

### Requirement 4: Orphan Branch Creation

**User Story:** As a developer, I want to create an orphan branch for development tooling, so that I can maintain tool dependencies separately from the main codebase.

#### Acceptance Criteria

1. WHEN creating an orphan branch, THE System SHALL verify the branch name does not already exist locally or remotely
2. WHEN creating an orphan branch, THE System SHALL stash any uncommitted changes in the current branch
3. WHEN an orphan branch is created, THE System SHALL initialize it with an empty working directory
4. WHEN an orphan branch is created, THE System SHALL create an initial commit to establish the branch
5. IF the user requests to return to the original branch after creation, THEN THE System SHALL restore the previous stash

### Requirement 5: Submodule Management in Orphan Branches

**User Story:** As a developer, I want to add submodules to an orphan branch with multi-URL support (SSH/HTTP), so that I can manage development tool dependencies using Git's native submodule system with automatic fallback when SSH is unavailable.

#### Acceptance Criteria

1. WHEN adding a submodule to an orphan branch, THE System SHALL verify the target branch is checked out
2. WHEN adding a submodule, THE System SHALL validate the submodule URL is accessible
3. WHEN adding a submodule, THE System SHALL clone the submodule repository to the specified path
4. WHEN adding a submodule with both SSH and HTTPS URLs, THE System SHALL store both URLs in .gitmodules using kog-* extension fields
5. WHEN adding multiple submodules, THE System SHALL process them sequentially and report progress for each
6. IF a submodule already exists at the target path, THEN THE System SHALL report an error and skip that submodule
7. WHEN Git native commands are executed, THE System SHALL preserve kog-* extension fields in .gitmodules
8. WHEN syncing submodule URLs, THE System SHALL configure local .git/config based on SSH availability
9. WHEN updating submodules, THE System SHALL attempt SSH first and automatically fallback to HTTPS if SSH fails

### Requirement 6: Integrated Workflow Orchestration

**User Story:** As a developer, I want a single command to execute the complete initialization workflow, so that I can set up a new repository efficiently without manual steps.

#### Acceptance Criteria

1. WHEN the workflow is invoked, THE Initialization_Workflow SHALL execute steps in the correct order: remote detection, main branch initialization, orphan branch creation, submodule addition
2. WHEN any step fails, THE Initialization_Workflow SHALL report the failure and stop execution
3. WHEN a step is skipped due to existing state, THE Initialization_Workflow SHALL log the skip reason and continue
4. THE Initialization_Workflow SHALL provide a dry-run mode that shows what would be done without making changes
5. THE Initialization_Workflow SHALL provide a summary report at the end showing what was created or modified

### Requirement 7: Safety and Validation

**User Story:** As a developer, I want the system to validate inputs and prevent destructive operations, so that I can use the tools confidently without risking data loss.

#### Acceptance Criteria

1. WHEN a destructive operation is requested, THE System SHALL require an explicit force flag with a verbose name
2. WHEN a force flag is used, THE System SHALL display a warning and wait 3 seconds before proceeding
3. WHEN validating URLs, THE System SHALL check for proper format and protocol
4. WHEN validating branch names, THE System SHALL reject names with invalid characters
5. WHEN validating paths, THE System SHALL ensure they do not conflict with existing files or directories
6. THE System SHALL validate that the current directory is a Git repository before performing Git operations

### Requirement 8: Error Handling and Recovery

**User Story:** As a developer, I want clear error messages with recovery instructions, so that I can resolve issues when operations fail.

#### Acceptance Criteria

1. WHEN an operation fails, THE System SHALL provide a descriptive error message explaining what went wrong
2. WHEN an operation fails, THE System SHALL suggest specific recovery steps
3. WHEN a stash operation fails, THE System SHALL provide commands to manually inspect and recover the stash
4. WHEN a remote operation fails, THE System SHALL distinguish between network errors, authentication errors, and repository errors
5. THE System SHALL log all operations to help with debugging and troubleshooting

### Requirement 9: Script Reusability and Modularity

**User Story:** As a developer, I want individual scripts for each operation, so that I can use them independently or compose them into custom workflows.

#### Acceptance Criteria

1. THE System SHALL provide a script for multi-remote setup that can be used independently
2. THE System SHALL provide a script for orphan branch creation that can be used independently
3. THE System SHALL provide kog-submodule commands for managing submodules with multi-URL support
4. THE System SHALL provide an integrated workflow script that orchestrates all operations
5. WHEN scripts are used independently, THE System SHALL validate prerequisites and report missing dependencies
6. THE System SHALL use shared helper functions from git-helpers.sh to avoid code duplication

### Requirement 10: Root Repo Multi-URL Configuration

**User Story:** As a developer, I want root repository multi-URL and multi-remote configuration to be stored in .gitmodules using extension fields, so that the configuration is version-controlled and shared across all machines without requiring a separate configuration file.

#### Acceptance Criteria

1. WHEN configuring root repo with multiple remotes and URLs, THE System SHALL store all remote configurations in .gitmodules using kog-root-remote-<name>-<protocol> fields
2. WHEN Git native commands (git submodule sync, git config -f .gitmodules) are executed, THE System SHALL preserve all kog-root-remote-* extension fields without modification
3. WHEN cloning a repository with kog-root-remote-* fields in .gitmodules, THE kog-submodule sync command SHALL read these fields and configure all remotes in .git/config
4. THE System SHALL use kog-root-remote- prefix for all extension fields to avoid namespace conflicts with Git's native fields
5. THE System SHALL support kog-root-protocol-priority with values: auto (default), ssh, https
6. WHEN kog-root-protocol-priority is auto, THE System SHALL detect SSH availability and prefer SSH, fallback to HTTPS if SSH unavailable
7. WHEN a root repo has only HTTPS URLs, THE System SHALL work correctly without attempting SSH
8. THE System SHALL support arbitrary remote names (origin, upstream, myfork, gitlab, etc.) without hardcoded assumptions
9. THE System SHALL maintain backward compatibility: repositories without kog-* fields SHALL work with standard Git commands
10. WHEN kog-submodule commands are not available, THE System SHALL allow users to use standard Git commands with the default url field

### Requirement 11: Submodule Multi-URL Configuration

### Requirement 11: Submodule Multi-URL Configuration

**User Story:** As a developer, I want submodule multi-URL and multi-remote configuration to be stored in .gitmodules using extension fields, so that the configuration is version-controlled and shared across all machines without requiring a separate configuration file.

#### Acceptance Criteria

1. WHEN adding a submodule with multiple remotes and URLs, THE System SHALL store all remote configurations in .gitmodules using kog-remote-<name>-<protocol> fields
2. WHEN Git native commands (git submodule update, git submodule sync, git config -f .gitmodules) are executed, THE System SHALL preserve all kog-remote-* extension fields without modification
3. WHEN cloning a repository with kog-remote-* fields in .gitmodules, THE kog-submodule sync command SHALL read these fields and configure all remotes in .git/modules/<submodule>/config
4. THE System SHALL use kog-remote- prefix for all extension fields to avoid namespace conflicts with Git's native fields
5. THE System SHALL support kog-protocol-priority with values: auto (default), ssh, https
6. WHEN kog-protocol-priority is auto, THE System SHALL detect SSH availability and prefer SSH, fallback to HTTPS if SSH unavailable
7. WHEN a submodule has only HTTPS URLs, THE System SHALL work correctly without attempting SSH
8. THE System SHALL support arbitrary remote names (origin, upstream, myfork, gitlab, etc.) without hardcoded assumptions
9. THE System SHALL maintain backward compatibility: repositories without kog-* fields SHALL work with standard Git commands
10. WHEN kog-submodule commands are not available, THE System SHALL allow users to use standard Git submodule commands with the default url field

### Requirement 12: Recursive Repository Configuration

**User Story:** As a developer, I want a repository that is both a root repo (with its own submodules) AND a submodule of another repo to work correctly, so that I can manage complex nested repository structures.

#### Acceptance Criteria

1. WHEN a repository is both a root repo and a submodule, THE System SHALL store root repo remotes using kog-root-remote-* prefix in the root repo's .gitmodules
2. WHEN a repository is both a root repo and a submodule, THE System SHALL store submodule remotes using kog-remote-* prefix in the super repo's .gitmodules
3. WHEN kog-submodule sync is executed, THE System SHALL configure root repo remotes from kog-root-remote-* fields and submodule remotes from kog-remote-* fields independently
4. THE System SHALL NOT require filesystem traversal or additional config files to determine configuration location
5. THE System SHALL maintain backward compatibility: repositories without kog-* fields SHALL work with standard Git commands
