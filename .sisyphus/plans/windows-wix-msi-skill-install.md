# Windows WiX MSI for kano-git Skill Install

## TL;DR
> **Summary**: Add a per-user WiX MSI that installs `kano-git-master-skill` into the fixed agent-readable skill path under `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`, blocks over an existing developer checkout, requires Git for Windows 2.44+, and offers optional shell integration.
> **Deliverables**:
> - WiX packaging subtree with `Product.wxs` and build assets
> - MSI prerequisite gating for Git for Windows / Bash / minimum Git version 2.44+
> - File deployment to fixed skill path with uninstall cleanup
> - Optional shell integration feature tied to existing launcher/install surfaces
> **Effort**: Large
> **Parallel**: YES - 4 waves
> **Critical Path**: install contract → WiX authoring skeleton → prerequisite/conflict gating → shell-integration feature → verification

## Context
### Original Request
- Plan WiX Toolset MSI packaging for `kano-git-master-skill`
- Install into agent-readable skill path, not an arbitrary path
- Consider future MSI while current distribution is still skill-like, not a conventional app installer
- Produce a `Product.wxs` plan and decide installer boundaries

### Interview Summary
- Install root is fixed to `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`
- If that target path already contains a manually cloned repo (developer mode), installer must stop instead of overwriting
- MSI shell integration should be an optional feature, not mandatory
- Git for Windows / `bash.exe` is a hard prerequisite; MSI must not bundle it
- Git for Windows minimum supported version is **2.44+**
- `kano-git-installer` is conceptually moving toward top-level `kog install/uninstall` orchestration, but current plan should work with existing launcher/install surfaces

### Metis Review (gaps addressed)
- Preserve shell as apply-layer only; durable install state remains native-owned
- Avoid scope creep into MSI-driven Git/Bash execution during install unless strictly necessary
- Treat developer checkout at target path as a hard conflict, not an upgrade scenario
- Keep per-user scope stable; do not design a mixed per-user/per-machine product
- Make acceptance criteria explicit for prerequisite failure, conflict abort, optional shell integration, and uninstall cleanup

## Work Objectives
### Core Objective
Create a decision-complete implementation path for a **per-user WiX MSI** that installs the skill into the canonical `.agents\skills` location and cleanly separates file deployment, install state, and optional shell integration.

### Deliverables
- `src/wix/Product.wxs`
- `src/wix/Fragments/Directories.wxs`
- `src/wix/Fragments/Components.wxs`
- `src/wix/Fragments/Features.wxs`
- `src/wix/Fragments/Prerequisites.wxs`
- `src/wix/build.cmd` or `src/wix/build.ps1`
- Optional generated harvest file if using Heat/HeatWave (`src/wix/generated/*.wxs`)
- Packaging documentation snippet explaining MSI vs portable install behavior

### Definition of Done (verifiable conditions with commands)
- `wix build src/wix/Product.wxs ...` (or equivalent scripted build) produces an MSI successfully
- MSI installs to `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`
- MSI aborts with actionable message when Git for Windows is missing
- MSI aborts with actionable message when `git --version` < `2.44`
- MSI aborts when target path already contains a developer checkout / existing repo content
- MSI uninstall removes installed files and installer-owned cleanup targets without touching developer checkout scenarios
- Optional shell integration feature can be enabled/disabled predictably

### Must Have
- Per-user MSI only
- Fixed install root under `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`
- Hard prerequisite: Git for Windows + `bash.exe` + Git version >= 2.44
- Optional shell integration feature
- MSI optional shell integration owns **per-user PATH only**
- No overwrite of manually cloned developer repo at target path
- Native install-state path remains `~/.kano/git/kog-install-state.json`

### Must NOT Have (guardrails, AI slop patterns, scope boundaries)
- Must NOT install to `Program Files`
- Must NOT support per-machine scope in this product
- Must NOT bundle Git for Windows inside MSI
- Must NOT rely on broad custom actions for ordinary file copy/PATH/uninstall cleanup when declarative WiX constructs suffice
- Must NOT overwrite target path when an existing manual repo/developer checkout is detected
- Must NOT preserve or read legacy `.kog-installed-marker`
- Must NOT conflate file deployment with shell integration in a way that makes silent install behavior ambiguous
- Must NOT have MSI write shell rc/profile completion hooks; completion/profile ownership stays outside MSI

## Verification Strategy
> ZERO HUMAN INTERVENTION — all verification is agent-executed.
- Test decision: tests-after + MSI build/install verification scripts
- QA policy: Every task includes agent-executed validation
- Evidence: `.sisyphus/evidence/task-{N}-{slug}.{ext}`

## Execution Strategy
### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = under-splitting.
> Extract shared dependencies as Wave-1 tasks for max parallelism.

Wave 1: contract + packaging skeleton + prerequisite model + conflict model + shell feature model
Wave 2: WiX directory/components/features authoring + build script + launcher/install integration adjustments
Wave 3: installer UX/messages + optional shell integration implementation + uninstall cleanup + docs/help alignment
Wave 4: build/install/uninstall verification + negative-case verification + regression sweep

### Dependency Matrix (full, all tasks)
- 1 blocks 6, 7, 8, 9
- 2 blocks 6, 7, 8
- 3 blocks 7, 10
- 4 blocks 7, 10
- 5 blocks 8, 11
- 6 blocks 12, 13
- 7 blocks 12, 13
- 8 blocks 12, 14
- 9 blocks 13
- 10 blocks 13
- 11 blocks 14
- 12, 13, 14 block final verification wave

### Agent Dispatch Summary (wave → task count → categories)
- Wave 1 → 5 tasks → writing / unspecified-high / quick
- Wave 2 → 6 tasks → unspecified-high / quick
- Wave 3 → 3 tasks → unspecified-high / writing
- Wave 4 → 3 tasks → unspecified-high / deep

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

- [x] 1. Freeze MSI install contract and file layout

  **What to do**: Define the exact MSI-owned filesystem contract: install root `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`, installed file set, non-installed/generated files, uninstall cleanup scope, and feature boundaries (`CoreFiles`, `ShellIntegration`). Record canonical directory ids and artifact mapping for WiX.
  **Must NOT do**: Must not leave install root configurable. Must not use `ProgramFilesFolder` or mixed per-user/per-machine semantics.

  **Recommended Agent Profile**:
  - Category: `writing` — Reason: contract/spec work with exact naming and scope boundaries
  - Skills: `[]` — no special skill required
  - Omitted: `playwright` — not a browser task

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: 6,7,8,9 | Blocked By: none

  **References**:
  - Pattern: `scripts/kano-git:1209-1277` — launcher help already documents install/completion surfaces
  - Pattern: `scripts/kano-git-installer:1-176` — current shell integration responsibilities
  - API/Type: `src/cpp/code/systems/kano_git_command/product/private/self_cmd.cpp:193-334` — native install-state ownership
  - External: `https://wixtoolset.org/docs/schema/wxs/packagescopetype` — per-user scope guidance

  **Acceptance Criteria**:
  - [ ] Plan docs define fixed install root and exact feature split with no ambiguous paths
  - [ ] No plan artifact refers to `Program Files` as install target

  **QA Scenarios**:
  ```
  Scenario: Contract paths are internally consistent
    Tool: Bash
    Steps: Search authored WiX plan/assets for INSTALLFOLDER and skill path identifiers; confirm single fixed root model
    Expected: All references resolve to the same per-user skill path model
    Evidence: .sisyphus/evidence/task-1-install-contract.txt

  Scenario: Forbidden install roots are absent
    Tool: Bash
    Steps: Search for ProgramFilesFolder/perMachine references in authored packaging assets
    Expected: No forbidden scope/root references remain
    Evidence: .sisyphus/evidence/task-1-install-contract-error.txt
  ```

  **Commit**: YES | Message: `feat(installer): define per-user skill MSI contract` | Files: `src/wix/**`, docs/help files as needed

- [x] 2. Create WiX packaging subtree and root Product authoring

  **What to do**: Add packaging subtree under `src/wix/` (not directly under `src/` root files) with `Product.wxs` as the entrypoint and fragment files for directories/components/features/prerequisites. Choose WiX version intentionally (prefer WiX v4) and wire package metadata, upgrade code placeholder strategy, media/cab behavior, and build orchestration script.
  **Must NOT do**: Must not put `Product.wxs` loose under `src/` without packaging subtree. Must not adopt the SCE sample’s per-machine shape.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: packaging architecture and file authoring
  - Skills: `[]`
  - Omitted: `git-master` — not a git-history task

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 6,7,8 | Blocked By: 1

  **References**:
  - Pattern: `H:\Dev\AutoSDK\HostWin64\PS5\11.00.00.45\WiX\SCECommonInstaller\Product.wxs` — use as WiX skeleton reference only, not scope/path model
  - Pattern: `H:\Dev\AutoSDK\HostWin64\PS5\11.00.00.45\WiX\SCECommonInstaller\build.sh` — reference for WiX asset build flow only
  - External: `https://wixtoolset.org/docs/schema/wxs/packagescopetype` — package scope docs

  **Acceptance Criteria**:
  - [ ] `src/wix/Product.wxs` exists and composes fragment files cleanly
  - [ ] Packaging subtree location is documented and consistent

  **QA Scenarios**:
  ```
  Scenario: Packaging subtree exists with expected entrypoints
    Tool: Bash
    Steps: List src/wix contents and verify Product.wxs plus supporting fragments/build script are present
    Expected: Expected WiX packaging files exist in a dedicated subtree
    Evidence: .sisyphus/evidence/task-2-wix-subtree.txt

  Scenario: No stray root-level Product.wxs placement
    Tool: Bash
    Steps: Search repo for Product.wxs outside src/wix
    Expected: No misplaced duplicate installer entrypoint exists
    Evidence: .sisyphus/evidence/task-2-wix-subtree-error.txt
  ```

  **Commit**: YES | Message: `feat(installer): scaffold wix packaging subtree` | Files: `src/wix/**`

- [x] 3. Implement Git for Windows prerequisite detection and minimum-version gate

  **What to do**: Author MSI launch conditions or equivalent deterministic checks for: presence of `git.exe`, presence of `bash.exe`, and parsed Git version >= `2.44`. Emit actionable failure guidance telling the user to install/update Git for Windows rather than attempting bundled install.
  **Must NOT do**: Must not silently continue without Git/Bash. Must not bundle Git in the MSI. Must not use vague error text.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: prerequisite gating and installer UX logic
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 7,10 | Blocked By: 1

  **References**:
  - Pattern: `scripts/kog.cmd:1-17` — current Bash dependency discovery on Windows
  - Pattern: `scripts/kano-git-installer.cmd:1-17` — installer wrapper Bash dependency
  - External: librarian research result `bg_8e3a7108` — Git/Bash should be prerequisite, not bundled

  **Acceptance Criteria**:
  - [ ] Missing Git/Bash prevents installation with explicit guidance
  - [ ] Git version below 2.44 prevents installation with explicit guidance

  **QA Scenarios**:
  ```
  Scenario: Missing Git prerequisite blocks install
    Tool: Bash
    Steps: Run MSI or prerequisite-check path in isolated env with git.exe/bash.exe hidden from PATH
    Expected: Install aborts with clear Git for Windows install guidance
    Evidence: .sisyphus/evidence/task-3-git-prereq-missing.txt

  Scenario: Old Git version blocks install
    Tool: Bash
    Steps: Mock or route prerequisite check to report git 2.43.x
    Expected: Install aborts and states minimum supported version is 2.44+
    Evidence: .sisyphus/evidence/task-3-git-prereq-old.txt
  ```

  **Commit**: YES | Message: `feat(installer): gate msi on git for windows 2.44+` | Files: `src/wix/**`

- [x] 4. Implement developer-mode repo conflict detection and abort flow

  **What to do**: Define and implement deterministic detection for an existing manual checkout at the target skill path. Abort install before file copy when target path already contains a repo/developer checkout. Provide explicit message that developer-mode checkout remains authoritative and MSI will not overwrite it.
  **Must NOT do**: Must not treat arbitrary existing files as safe to overwrite. Must not try in-place upgrade against a manual clone.

  **Recommended Agent Profile**:
  - Category: `unspecified-high`
  - Skills: `[]`
  - Omitted: `git-master`

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 7,10 | Blocked By: 1

  **References**:
  - Pattern: target root `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`
  - Pattern: repo structure itself (`.git`, `scripts/`, `src/cpp/`) should inform conflict detection heuristics
  - External: user decision — developer checkout must block installation

  **Acceptance Criteria**:
  - [ ] Existing developer checkout at target path aborts install before overwrite
  - [ ] Error message explains why install stopped and what user can do next

  **QA Scenarios**:
  ```
  Scenario: Developer checkout conflict blocks install
    Tool: Bash
    Steps: Create target path with .git and representative repo files, launch installer
    Expected: Installer aborts before copying files and reports developer-mode conflict
    Evidence: .sisyphus/evidence/task-4-dev-conflict.txt

  Scenario: Clean target path allows progress
    Tool: Bash
    Steps: Ensure target path absent or empty, launch installer up to file deployment stage
    Expected: Conflict gate does not block installation
    Evidence: .sisyphus/evidence/task-4-dev-conflict-clear.txt
  ```

  **Commit**: YES | Message: `feat(installer): block over developer checkout` | Files: `src/wix/**`

- [x] 5. Define optional shell integration feature semantics

  **What to do**: Specify exactly what the MSI’s optional shell integration feature does and does not do. **Decision**: MSI `ShellIntegration` owns only the per-user PATH entry for command availability plus post-install guidance. Completion files and shell profile hooks remain owned by launcher-side commands (`kog completion install` / future `kog install`). Reconcile docs/help around this ownership split.
  **Must NOT do**: Must not leave overlap ambiguous. Must not have MSI and shell installer fighting over the same rc blocks without ownership rules.

  **Recommended Agent Profile**:
  - Category: `writing`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: 8,11 | Blocked By: 1

  **References**:
  - Pattern: `scripts/kano-git-installer:1-176` — current full shell integration behavior
  - Pattern: `scripts/kano-git:795-859` — current completion-only install surface
  - External: WiX per-user PATH via `Environment System="no"`

  **Acceptance Criteria**:
  - [ ] Feature semantics document clearly distinguishes MSI PATH-only shell feature vs `kog completion install` vs `self install`
  - [ ] Installer UI/feature text matches actual behavior

  **QA Scenarios**:
  ```
  Scenario: Feature documentation is behaviorally aligned
    Tool: Bash
    Steps: Compare authored MSI feature text against launcher help/install docs
    Expected: No conflicting ownership claims remain; MSI is described as PATH-only for shell integration
    Evidence: .sisyphus/evidence/task-5-shell-feature.txt

  Scenario: Completion-only path remains distinct
    Tool: Bash
    Steps: Inspect help text and installer docs for completion install wording
    Expected: Completion-only command is not described as full install path
    Evidence: .sisyphus/evidence/task-5-shell-feature-error.txt
  ```

  **Commit**: YES | Message: `docs(installer): define shell integration feature boundary` | Files: docs/help + `src/wix/**`

- [x] 6. Author WiX directory tree for fixed per-user skill root

  **What to do**: Implement the WiX directory tree using a per-user root (`UserProfileFolder` or explicit per-user equivalent) ending at `.agents\skills\kano\kano-git-master-skill`. Define child directories for scripts, src payload, binaries, and any packaging-owned folders.
  **Must NOT do**: Must not use `ProgramFilesFolder`. Must not author a configurable install root.

  **Recommended Agent Profile**:
  - Category: `quick` — Reason: concrete WiX structure authoring once contract is fixed
  - Skills: `[]`
  - Omitted: `oracle`

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 12,13 | Blocked By: 1,2

  **References**:
  - External: librarian research `bg_8e3a7108` — per-user WiX directory patterns
  - Pattern: current repo structure from project root

  **Acceptance Criteria**:
  - [ ] Directory ids map exactly to the fixed skill root and payload layout
  - [ ] Install path resolves under `%USERPROFILE%` only

  **QA Scenarios**:
  ```
  Scenario: WiX directories resolve to fixed skill root
    Tool: Bash
    Steps: Inspect Directory tree in Product/Fragments and confirm all paths descend from user profile root to .agents\skills\kano\kano-git-master-skill
    Expected: Single deterministic install root is authored
    Evidence: .sisyphus/evidence/task-6-wix-dirs.txt

  Scenario: No configurable INSTALLFOLDER override remains
    Tool: Bash
    Steps: Search WiX sources for UI or properties exposing install path edits
    Expected: No custom-path UI/property is present
    Evidence: .sisyphus/evidence/task-6-wix-dirs-error.txt
  ```

  **Commit**: YES | Message: `feat(installer): author fixed per-user wix directories` | Files: `src/wix/Fragments/Directories.wxs`, `src/wix/Product.wxs`

- [x] 7. Author prerequisite and conflict gates in WiX

  **What to do**: Implement launch conditions / searches / prerequisite fragments for Git presence, Bash presence, minimum Git version 2.44+, and developer-checkout conflict abort. Ensure messages are user-actionable and do not require reading logs to understand failure.
  **Must NOT do**: Must not hide failures behind generic MSI error codes without clear text.

  **Recommended Agent Profile**:
  - Category: `unspecified-high`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 12,13 | Blocked By: 1,2,3,4

  **References**:
  - Pattern: `scripts/kog.cmd:6-16` — Bash discovery expectations
  - External: WiX prerequisite research `bg_8e3a7108`

  **Acceptance Criteria**:
  - [ ] MSI launch blocks with specific message for missing Git/Bash
  - [ ] MSI launch blocks with specific message for Git < 2.44
  - [ ] MSI launch blocks with specific message for developer-checkout conflict

  **QA Scenarios**:
  ```
  Scenario: Prerequisite matrix produces correct failure messages
    Tool: Bash
    Steps: Run gated install checks under mocked missing/old/conflict conditions
    Expected: Each condition yields the intended distinct message and aborts before file install
    Evidence: .sisyphus/evidence/task-7-gates.txt

  Scenario: Happy path prerequisites permit install
    Tool: Bash
    Steps: Run install with Git 2.44+, bash present, clean target path
    Expected: No prerequisite/conflict gate blocks execution
    Evidence: .sisyphus/evidence/task-7-gates-happy.txt
  ```

  **Commit**: YES | Message: `feat(installer): add wix prerequisite and conflict gates` | Files: `src/wix/Fragments/Prerequisites.wxs`, related sources

- [x] 8. Author MSI feature/component model including optional shell integration

  **What to do**: Create WiX features/components for `CoreFiles` and `ShellIntegration`. `CoreFiles` installs skill payload and wrappers. `ShellIntegration` is optional and owns **only** a per-user PATH update via WiX `Environment` rows. It does not install completion files, write rc/profile hooks, or call shell installers.
  **Must NOT do**: Must not place shell integration into core feature unconditionally.

  **Recommended Agent Profile**:
  - Category: `unspecified-high`
  - Skills: `[]`
  - Omitted: `oracle`

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 12,14 | Blocked By: 1,2,5

  **References**:
  - Pattern: `scripts/kog.cmd`, `scripts/kano-git.cmd`, `scripts/kog-installer.cmd`, `scripts/kano-git-installer.cmd`
  - External: WiX Environment patterns from `bg_8e3a7108`

  **Acceptance Criteria**:
  - [ ] MSI exposes optional shell integration feature distinctly from core files
  - [ ] PATH update is per-user only if feature is enabled
  - [ ] No completion/profile hook artifacts are owned by MSI

  **QA Scenarios**:
  ```
  Scenario: Install with shell integration enabled adds expected user-level integration
    Tool: Bash
    Steps: Install MSI with shell feature enabled in test profile
    Expected: Core files install and only the per-user PATH integration is applied by MSI
    Evidence: .sisyphus/evidence/task-8-shell-feature-on.txt

  Scenario: Install with shell integration disabled leaves shell untouched
    Tool: Bash
    Steps: Install MSI with shell feature disabled in clean test profile
    Expected: Core files install, but no PATH mutation or other shell integration artifacts are applied
    Evidence: .sisyphus/evidence/task-8-shell-feature-off.txt
  ```

  **Commit**: YES | Message: `feat(installer): split core files and shell integration features` | Files: `src/wix/Fragments/Features.wxs`, `src/wix/Fragments/Components.wxs`

- [x] 9. Add WiX build automation and deterministic payload inclusion

  **What to do**: Author build automation for the MSI (prefer `build.cmd` or `build.ps1` under `src/wix/`). Decide whether payload is hand-authored or harvested. If using harvest, ensure generated files live in a dedicated generated subtree and are reproducible.
  **Must NOT do**: Must not depend on ad hoc manual WiX commands only. Must not scatter generated files unpredictably.

  **Recommended Agent Profile**:
  - Category: `quick`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: 13 | Blocked By: 1,2

  **References**:
  - Pattern: `H:\Dev\AutoSDK\HostWin64\PS5\11.00.00.45\WiX\SCECommonInstaller\build.sh` — reference only for tool invocation style

  **Acceptance Criteria**:
  - [ ] Single documented build command produces MSI from repo root or packaging root
  - [ ] Generated payload authoring is deterministic and checked into the intended location if required

  **QA Scenarios**:
  ```
  Scenario: Build automation produces MSI
    Tool: Bash
    Steps: Run documented WiX build command/script from clean checkout
    Expected: MSI artifact is produced successfully at documented output path
    Evidence: .sisyphus/evidence/task-9-build-msi.txt

  Scenario: Rebuild is deterministic
    Tool: Bash
    Steps: Run build twice without source changes and compare expected authored/generated asset set
    Expected: No unexpected file churn outside intended outputs
    Evidence: .sisyphus/evidence/task-9-build-msi-error.txt
  ```

  **Commit**: YES | Message: `build(installer): add wix build automation` | Files: `src/wix/**`

- [x] 10. Align native and launcher help/install wording with MSI model

  **What to do**: Update command/help/documentation text so users understand the difference between `kog install/uninstall` (future orchestrator or current shell integration), `self install` (install state), `completion install` (completion only), and MSI install behavior. Add explicit note that MSI is per-user skill deployment and that existing manual repo at target path blocks installation.
  **Must NOT do**: Must not leave old “marker” terminology anywhere in user-facing text.

  **Recommended Agent Profile**:
  - Category: `writing`
  - Skills: `[]`
  - Omitted: `oracle`

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 13 | Blocked By: 3,4,7

  **References**:
  - Pattern: `scripts/kano-git:1209-1295` — launcher help extras
  - Pattern: `src/cpp/code/systems/kano_git_command/product/private/self_cmd.cpp` — native subcommand descriptions

  **Acceptance Criteria**:
  - [ ] No remaining user-facing “marker” terminology for install state
  - [ ] Help text clearly distinguishes MSI/core install/shell integration/completion-only flows

  **QA Scenarios**:
  ```
  Scenario: Help text is terminology-consistent
    Tool: Bash
    Steps: Run `./scripts/kog --help` and inspect install-related sections
    Expected: “install state” terminology is used consistently and boundaries are clear
    Evidence: .sisyphus/evidence/task-10-help.txt

  Scenario: MSI docs mention developer conflict abort
    Tool: Bash
    Steps: Search packaging docs/help for manual repo conflict behavior
    Expected: User-facing explanation exists
    Evidence: .sisyphus/evidence/task-10-help-error.txt
  ```

  **Commit**: YES | Message: `docs(installer): align help with msi model` | Files: launcher/native help docs

- [x] 11. Decide and implement shell-integration ownership boundary after MSI install

  **What to do**: Implement the chosen ownership decision consistently: **MSI owns only per-user PATH**, while `kog completion install` / future `kog install` own completion files and profile hooks. Encode this split consistently in installer authoring, launcher docs, and uninstall behavior.
  **Must NOT do**: Must not leave duplicated ownership where MSI and launcher both write the same rc block without coordination.

  **Recommended Agent Profile**:
  - Category: `deep`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: 14 | Blocked By: 5,8

  **References**:
  - Pattern: `scripts/kano-git-installer` — current shell integration owner
  - Pattern: `scripts/kano-git:795-859` — completion-only install owner
  - External: user decision — shell integration is optional in MSI

  **Acceptance Criteria**:
  - [ ] Exactly one owner is defined per shell-integration artifact type
  - [ ] Uninstall removes only installer-owned artifacts and leaves non-owned artifacts untouched

  **QA Scenarios**:
  ```
  Scenario: Ownership matrix is unambiguous
    Tool: Bash
    Steps: Review installer docs/assets against launcher installer logic
    Expected: PATH is MSI-owned; completion files and rc hooks are launcher-owned; install state remains native-owned
    Evidence: .sisyphus/evidence/task-11-ownership.txt

  Scenario: Uninstall respects ownership rules
    Tool: Bash
    Steps: Install then uninstall with optional shell feature enabled; inspect remaining artifacts
    Expected: MSI removes only PATH mutation it owns; completion/profile artifacts remain under launcher ownership unless separately managed
    Evidence: .sisyphus/evidence/task-11-ownership-error.txt
  ```

  **Commit**: YES | Message: `refactor(installer): define shell integration ownership` | Files: `src/wix/**`, installer/launcher docs or code

- [x] 12. Verify MSI build, install, uninstall, and upgrade in the happy path

  **What to do**: Run the built MSI in a clean test profile with Git for Windows >= 2.44 and no pre-existing target repo. Verify installed files, wrappers, optional shell feature behavior, install-state file, and uninstall cleanup. Validate upgrade path within the same per-user context.
  **Must NOT do**: Must not treat install success without uninstall verification as complete.

  **Recommended Agent Profile**:
  - Category: `unspecified-high`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: final verification wave | Blocked By: 6,7,8

  **References**:
  - Generated MSI artifact path from task 9
  - Install-state path `~/.kano/git/kog-install-state.json`

  **Acceptance Criteria**:
  - [ ] Happy-path install succeeds
  - [ ] Happy-path uninstall succeeds and cleans installer-owned artifacts
  - [ ] Same-scope upgrade succeeds

  **QA Scenarios**:
  ```
  Scenario: Happy path install/uninstall/upgrade succeeds
    Tool: Bash
    Steps: Install MSI, inspect installed tree, rerun newer MSI for upgrade, uninstall
    Expected: All operations succeed within per-user scope with expected artifacts
    Evidence: .sisyphus/evidence/task-12-msi-happy.txt

  Scenario: Uninstall leaves no installer-owned debris
    Tool: Bash
    Steps: After uninstall, inspect install root and install-state location
    Expected: Installer-owned files are removed according to ownership model
    Evidence: .sisyphus/evidence/task-12-msi-happy-error.txt
  ```

  **Commit**: NO | Message: `n/a` | Files: none

- [x] 13. Verify prerequisite and conflict negative paths end-to-end

  **What to do**: Execute negative tests for missing Git, missing Bash, old Git version (<2.44), and developer checkout at target path. Capture exact messages and ensure failure happens before destructive actions.
  **Must NOT do**: Must not accept generic MSI failure codes as sufficient evidence.

  **Recommended Agent Profile**:
  - Category: `deep`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: final verification wave | Blocked By: 6,7,9,10

  **References**:
  - Prerequisite and conflict gates from task 7

  **Acceptance Criteria**:
  - [ ] Each negative path fails with distinct, actionable message
  - [ ] No partial install occurs on gated failure

  **QA Scenarios**:
  ```
  Scenario: Missing/old Git and missing Bash are blocked cleanly
    Tool: Bash
    Steps: Run MSI under each mocked prerequisite failure condition
    Expected: Installer stops before file copy and prints expected guidance
    Evidence: .sisyphus/evidence/task-13-msi-negative.txt

  Scenario: Developer checkout conflict is blocked cleanly
    Tool: Bash
    Steps: Seed target path with manual repo, run installer
    Expected: Installer stops without overwriting any repo content
    Evidence: .sisyphus/evidence/task-13-msi-negative-conflict.txt
  ```

  **Commit**: NO | Message: `n/a` | Files: none

- [x] 14. Verify optional shell integration feature behavior and ownership boundaries

  **What to do**: Run install/uninstall with shell feature toggled on and off. Confirm PATH/completion/profile mutations exactly match the chosen ownership model and that completion-only commands remain usable separately.
  **Must NOT do**: Must not leave silent shell mutations when feature is disabled.

  **Recommended Agent Profile**:
  - Category: `unspecified-high`
  - Skills: `[]`
  - Omitted: `playwright`

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: final verification wave | Blocked By: 8,11

  **References**:
  - Pattern: `scripts/kano-git-installer`
  - Pattern: `scripts/kano-git:795-859`

  **Acceptance Criteria**:
  - [ ] Feature enabled case applies only intended shell integration artifacts
  - [ ] Feature disabled case leaves shell integration untouched
  - [ ] Separate completion-only command still works as documented

  **QA Scenarios**:
  ```
  Scenario: Shell feature enabled behaves as designed
    Tool: Bash
    Steps: Install with feature enabled, inspect PATH/profile/completion outcomes
    Expected: Only designed shell artifacts are present and owned correctly
    Evidence: .sisyphus/evidence/task-14-shell-qa-on.txt

  Scenario: Shell feature disabled preserves shell cleanliness
    Tool: Bash
    Steps: Install with feature disabled, inspect PATH/profile/completion outcomes
    Expected: No unintended shell mutation occurs
    Evidence: .sisyphus/evidence/task-14-shell-qa-off.txt
  ```

  **Commit**: NO | Message: `n/a` | Files: none

## Final Verification Wave (4 parallel agents, ALL must APPROVE)
- [ ] F1. Plan Compliance Audit — oracle
- [ ] F2. Code Quality Review — unspecified-high
- [ ] F3. Real Manual QA — unspecified-high (+ playwright if UI)
- [ ] F4. Scope Fidelity Check — deep

## Commit Strategy
- Commit 1: scaffold WiX packaging subtree and contract docs
- Commit 2: add prerequisite/conflict gates and fixed per-user directory tree
- Commit 3: add feature/component model and build automation
- Commit 4: align installer/help surfaces and ownership semantics
- Verification commits only if implementation changes are required after testing

## Success Criteria
- MSI installs skill files to the fixed `.agents\skills` path with per-user scope
- MSI refuses to install when Git for Windows/Bash is missing or Git < 2.44
- MSI refuses to overwrite a manual developer checkout at target path
- Optional shell integration is implemented as MSI-owned per-user PATH only, with unambiguous ownership
- Install state remains `kog-install-state.json` and no legacy marker path remains
- Build/install/uninstall/upgrade flows are reproducible and fully verified
