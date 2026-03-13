# TUI Branch Transplant Workflows

**Date**: 2026-03-13  
**Status**: Planning  
**Scope**: `kog tui` branch-to-branch transplant UX for cherry-pick and rebase assist

---

## Overview

This document defines the planning topic, UX contract, work items, test strategy, and phased delivery plan for branch-to-branch transplant workflows in the native C++ TUI.

The user goal is to make `kog tui` capable of:

- comparing commit history across two branches inside one selected repo,
- selecting one or more commits to transplant,
- executing cherry-pick and rebase workflows with clear progress,
- surfacing conflicts in a safe and understandable way,
- supporting manual conflict resolution outside the TUI while keeping the TUI as the operation monitor and control surface.

This document is planning-only. It does **not** authorize implementation shortcuts that bypass the UX and safety contracts described below.

---

## Goals

1. Add a keyboard-first TUI workflow for branch-to-branch commit transplant operations.
2. Make cherry-pick and rebase understandable without assuming deep Git internals knowledge.
3. Provide explicit, recoverable operation states for in-progress cherry-pick and rebase sequences.
4. Make conflicts actionable by showing the current operation, affected commit, unresolved files, and exact next steps.
5. Keep the TUI deterministic and safe by using Git itself as the source of truth for operation state.

---

## Non-Goals (v1)

- No inline merge editor inside the TUI.
- No PNG/image-based UI.
- No automatic force push or hidden history rewrite repairs.
- No silent auto-stage of unrelated files.
- No fully automated conflict resolution.
- No cross-repo transplant workflow in one action; scope is one selected repo at a time.

---

## Grounding in Current Code

Primary file:

- `src/cpp/code/systems/kano_git_command/ui/private/tui_dashboard_runner.cpp`

Existing relevant state and helpers already present:

- `CherryPickCandidate`
- `CherryPickPreflightState`
- `CherryPickRunnerState`
- `RebasePreflightState`
- `RebasePlannerState`
- `RebaseRunnerState`
- `ListBranches()`
- `CommitFileSet()`
- `HasPatchEquivalentInTarget()`
- `BuildCherryPickPreflight()`
- `CollectRebasePreflight()`
- `BuildRebasePlanner()`
- `RunRebaseContinue()` / `RunRebaseSkip()` / `RunRebaseAbort()`
- `RunRebaseStep()`
- `RunCherryPickOne()`
- `CherryPickContinue()` / `CherryPickSkip()` / `CherryPickAbort()`

Existing TUI patterns that this work must reuse:

- right-side dominant panel rendering,
- async background operations via `AsyncWorkState` and `screen.PostEvent(Event::Custom)`,
- keyboard-first control model,
- history page/detail model,
- preview/result panel model.

Current gap:

- the code has preflight/planner/runner pieces, but the UX contract is incomplete,
- conflict handling is currently too raw,
- there is no clear dual-branch history compare flow,
- operation recovery/re-entry is not yet a first-class concept,
- branch-aware history caching is not yet designed.

---

## Topic

**Topic name**: TUI Branch Transplant Workflows  
**Feature area**: `kog tui`  
**Operator domain**: cherry-pick + rebase assist

This topic covers all TUI interactions where a user wants to move commits from one branch context to another inside the selected repo.

---

## User Problems to Solve

### 1. Cherry-pick across two histories

The user needs to see source and target branch context clearly enough to choose the right commit(s) without guessing.

### 2. Rebase planning inside the TUI

The user needs a structured way to inspect local commits, choose actions (`pick`, `squash`, `fixup`, `drop`), and understand the resulting plan before running it.

### 3. Conflict interruption

When an operation stops on conflict, the current raw runner model is not enough. The TUI must clearly answer:

- what operation is in progress,
- which commit caused the stop,
- which files are unresolved,
- what the user must do next,
- which resume/skip/abort actions are available.

### 4. Recovery and continuity

If the user closes the panel, restarts `kog tui`, or resumes later, the TUI must be able to rediscover and re-present the active operation.

---

## UX Principles

1. **Keyboard-first**: no mouse dependency.
2. **One selected repo at a time**: branch transplant workflows always operate in the currently selected repo.
3. **Git is the state engine**: the TUI reflects Git's actual in-progress state instead of inventing shadow truth.
4. **Operation-first visibility**: when cherry-pick or rebase is active, the operation panel becomes the most important screen.
5. **Safe by default**: warn early, do not auto-force, do not silently mutate unrelated files.
6. **Manual conflict resolution in v1**: the TUI explains and coordinates; the user resolves conflicts in shell/editor/IDE.
7. **Recoverable flow**: closing a panel must not destroy the operation context.

---

## High-Level UX Model

### Core screens

1. **Branch Compare Screen**
   - selected repo only
   - source branch and target branch visible together
   - commit list and diff preview
   - supports single commit selection and range selection

2. **Cherry-pick Preflight Screen**
   - source branch
   - target branch
   - candidate commits (`target..source`)
   - duplicate-equivalent markers
   - risk markers
   - planned queue preview

3. **Cherry-pick Runner Screen**
   - current commit
   - queue progress
   - live output / last output
   - dominant action hints
   - conflict state handoff

4. **Rebase Preflight Screen**
   - current branch
   - base/upstream ref
   - merge base
   - candidate commits
   - risk and notes

5. **Rebase Planner Screen**
   - editable todo-style plan
   - actions: `pick`, `squash`, `fixup`, `drop`
   - preview text

6. **Conflict Resolution Screen**
   - operation type
   - repo path
   - current commit
   - unresolved file list
   - next-step checklist
   - continue / skip / abort actions

7. **Recovery / Resume Screen**
   - shown when Git indicates in-progress cherry-pick or rebase at startup or on refresh
   - offer inspect / resume / abort

---

## State Model

### Problem with current shape

Current runner states use booleans such as `waitingConflictResolution`. That is too weak for a multi-step workflow.

### Planned explicit runner state

Each operation runner should move to an explicit state enum.

```cpp
enum class RunnerState {
    Idle,
    Preflight,
    Planning,
    Ready,
    Running,
    ConflictDetected,
    ConflictGuidance,
    ReadyToContinue,
    Completed,
    Aborted,
    Failed
};
```

### Planned operation state invariants

- `Running` => current queue index is valid.
- `ConflictDetected` / `ConflictGuidance` => unresolved file list is non-empty or Git reports operation-in-progress stop state.
- `ReadyToContinue` => no unresolved entries remain.
- `Completed` => queue index equals queue size.
- `Aborted` => Git operation state is no longer active.

### Planned UI mode addition

Current TUI modes already include normal/command/help/palette/confirm style behavior. This topic will add a dedicated conflict-facing mode or equivalent dominant screen contract so that manual resolution guidance is not mixed with generic command mode.

---

## Branch Compare UX Plan

### Why this matters

Cherry-pick across branches is hard if the user cannot clearly compare source and target context.

### Planned compare model

- repo scope: selected repo only
- branch scope: explicit `source` and `target`
- default target: current checked out branch
- default source: chosen by user from local branches first; remote branches may be added later

### Planned compare layout (v1)

- left rail: branch selectors and compare summary
- center list: source-side candidate commits
- right panel: diff / details for current commit or selected range
- footer / header: current compare mode, selected range, action hints

### Selection modes

1. **Single commit selection**
2. **Contiguous range selection**
3. **Multi-select by marking individual commits** (optional if low-cost after range mode)

### Minimum v1 selection promise

Single selection and contiguous range selection must be supported before multi-mark workflows.

---

## Cherry-pick UX Plan

### Preflight data

For the selected repo, preflight must show:

- source branch
- target branch
- candidate commit list from `target..source`
- whether each commit has a patch-equivalent already in target
- simple risk tier from touched file count or other deterministic heuristics
- final queued commit order

### Runner contract

Once started, the runner panel becomes dominant and must show:

- operation: cherry-pick
- repo path
- source and target branch labels if known
- queue progress (`current/total`)
- current commit SHA and title
- current state (`running`, `waiting conflict guidance`, `ready to continue`, `completed`, `aborted`, `failed`)
- last Git output

### Conflict handoff contract

When a cherry-pick stops on conflict:

1. Runner state changes to `ConflictDetected`.
2. TUI gathers unresolved file info.
3. TUI enters `ConflictGuidance` display.
4. TUI shows exact instructions:
   - open shell/editor/IDE,
   - resolve conflicted files,
   - `git add <resolved-files>`,
   - return and continue, or skip, or abort.
5. TUI must not auto-stage unrelated files.
6. TUI may offer explicit stage shortcuts later, but not as an invisible side effect.

---

## Rebase UX Plan

### Preflight data

Rebase preflight must show:

- repo path
- current branch
- upstream/base ref
- merge base
- tracking summary
- candidate commits
- risk and note summary

### Planner contract

Rebase planner must support:

- per-item action change
- current selected line
- preview of the effective todo plan
- clear operator control hints

### Runner contract

Rebase runner mirrors cherry-pick runner, but labels the operation as rebase and shows plan item action where relevant.

### v1 simplification

The planner may internally execute `pick`/`squash`/`fixup` through a staged deterministic sequence rather than full interactive todo-file editing, as long as the user-facing plan remains explicit and recoverable.

---

## Conflict Resolution UX Contract

### v1 contract

Conflict resolution happens **outside** the TUI. The TUI is the control surface.

### Mandatory information on the conflict screen

- operation type
- repo path
- source/target/base context if known
- current commit SHA + title
- unresolved file list
- latest Git output snippet
- exact next-step checklist
- actions available now: continue / skip / abort / close panel

### Mandatory wording intent

The user should be able to understand the next step without Git expertise.

Example shape:

```text
state: conflict detected
current: abc1234 feat: port auth flow

unresolved files:
- src/auth/login.cpp
- src/auth/session.hpp

next steps:
1. Open your editor or shell and resolve the conflicted files.
2. Stage the resolved files with git add.
3. Return here and press continue.

actions:
- continue
- skip current commit
- abort operation
```

### v1 forbidden behavior

- no inline merge block editing,
- no silent auto-stage of unrelated files,
- no hidden reset/abort on panel close,
- no assumption that `git add .` is always correct.

---

## Recovery / Resume Contract

The TUI must be able to detect active Git operation state from the repo itself.

### Detectable signals

- `CHERRY_PICK_HEAD`
- rebase metadata directories/files
- unresolved index entries
- sequencer/todo state where available

### Planned recovery behavior

On startup, refresh, or when opening branch-transplant screens:

1. inspect Git operation state,
2. if an operation is already active, show a recovery banner or panel,
3. offer resume / inspect / abort,
4. restore the dominant operation screen instead of pretending the repo is idle.

### Close behavior

- `q` closes the panel only,
- operation continues to exist until Git says otherwise,
- reopening the panel should reconstruct the state from Git.

---

## Safety Rules

1. Warn or block when the working tree is dirty before starting transplant operations.
2. Warn when target branch is `main` / `master` or otherwise protected-like.
3. Warn when a commit appears patch-equivalent in target.
4. Never auto-force.
5. Never hide Git failure output.
6. Record original head for possible undo/recovery hints.
7. Do not treat a closed panel as a cancelled operation.

---

## Caching and Data Model Work

### Branch-aware history caching

Current history cache is effectively repo-path-oriented. Branch transplant workflows need branch-aware history and details caching.

### Planned direction

Introduce a key shape equivalent to:

- repo path
- ref / branch name
- maybe compare mode if needed

This avoids reusing one branch's cached history while the user is browsing another branch.

---

## Detailed Work Items

### Workstream A — operation state foundation

1. Replace boolean conflict-wait flags with explicit runner state enum.
2. Define state transition helpers and invariants.
3. Add operation-state detection from Git repo metadata.

### Workstream B — branch compare model

1. Add source/target branch selection model.
2. Add branch-aware history caching.
3. Add compare screen layout and selection behavior.

### Workstream C — cherry-pick workflow

1. Refine candidate enumeration and duplicate detection.
2. Add queue preview.
3. Upgrade runner panel to explicit operation-first UI.
4. Add conflict guidance screen.

### Workstream D — rebase workflow

1. Refine preflight risk output.
2. Harden planner editing and preview.
3. Upgrade runner panel to explicit operation-first UI.
4. Reuse conflict guidance model.

### Workstream E — recovery and undo hints

1. Detect active operation on startup and refresh.
2. Re-enter the correct panel from repo state.
3. Surface original head / undo hints after success.

### Workstream F — testing and docs

1. Add unit/property/integration coverage.
2. Add user-facing help copy inside TUI.
3. Update docs after implementation lands.

---

## Test Strategy

Test lanes already exist under `src/cpp/code/tests/kano_git_tui_tests/` with:

- `unit/`
- `property/`
- `integration/`

### Required test categories

#### 1. Unit tests

- candidate enumeration helpers
- duplicate detection helpers
- unresolved-file parsing helpers
- state transition validation
- queue progress calculations
- branch compare mapping helpers

#### 2. Property tests

- state machine never enters invalid state,
- runner invariants hold across random transition sequences,
- selection math remains stable across page and branch switches.

#### 3. Integration tests

- cherry-pick preflight from known branch fixture,
- cherry-pick conflict flow,
- cherry-pick abort flow,
- rebase planner flow,
- recovery after simulated in-progress operation,
- reopen panel and reconstruct state from Git metadata.

### Example acceptance-test intent

- `CherryPickRunner_ConflictFlow_EntersConflictGuidance`
- `CherryPickRunner_ContinueAfterResolved_CompletesQueue`
- `RebasePlanner_ActionEdits_UpdatePreview`
- `OperationRecovery_ExistingCherryPickState_ReopensRunner`
- `BranchHistoryCache_SwitchRef_UsesIndependentEntries`

---

## Atomic Commit Plan

### Commit 1

`refactor: introduce explicit runner state for branch transplant workflows`

- add runner state enum
- preserve behavior with minimal UI change
- add unit tests for state transitions

### Commit 2

`feat: detect in-progress cherry-pick and rebase state from git metadata`

- repo-state detection helpers
- startup/reopen integration tests

### Commit 3

`feat: add branch-aware history cache for transplant planning`

- history cache key changes
- branch switch tests

### Commit 4

`feat: add branch compare and cherry-pick preflight workflow`

- source/target selection
- candidate list UI
- duplicate/risk display

### Commit 5

`feat: add cherry-pick runner conflict guidance panel`

- dominant operation screen
- unresolved file display
- continue/skip/abort guidance

### Commit 6

`feat: upgrade rebase planner and runner workflow`

- planner polish
- runner state UI
- conflict guidance reuse

### Commit 7

`docs: document tui branch transplant workflows`

- help text
- docs updates
- examples if needed

---

## Delivery Phases

### Phase 0 — planning only

- finalize this document
- review for missing UX and safety edges

### Phase 1 — state and recovery foundation

- explicit runner states
- repo-state detection
- recovery entry points

### Phase 2 — branch compare and cherry-pick preflight

- source/target compare model
- branch-aware cache
- queue preview

### Phase 3 — cherry-pick runner and conflict guidance

- dominant operation panel
- conflict guidance contract
- continue/skip/abort polish

### Phase 4 — rebase planner and runner hardening

- planner UX
- runner UX
- conflict/recovery reuse

### Phase 5 — docs, help, and polish

- in-TUI hints
- docs updates
- test expansion

---

## Open Decisions (Keep Explicit)

These are design decisions to resolve during implementation planning, not reasons to skip the work:

1. Whether v1 branch compare uses side-by-side branch lists or a single branch-focused list with source/target headers.
2. Whether v1 supports multi-mark commit selection or only single + contiguous range.
3. Whether continue should become available automatically when unresolved entries reach zero, or only after explicit refresh/probe.
4. Whether undo after completion is only a hint or includes a first-class guided action.

---

## Recommended Next Execution Step

Use this document as the canonical planning artifact, then implement in the atomic order above, with tests added before behavior changes where practical.

Before any code changes for this topic:

1. review this plan,
2. confirm the state model and v1 conflict handoff contract,
3. start with the state/recovery foundation rather than with visual polish.
