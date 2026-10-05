# KOG-TSK-0142 independent overlap checkpoint

Date: 2026-09-30 (Asia/Taipei). Item UID:
`01a0eb3a-8225-700d-bc4b-e3f923b9b41e`. State remains `InProgress`.

## Owner implementation and scope

The existing owner worktree is `D:/_work/_Horizon/_worktrees/kog-tsk-0142`,
branch `codex/kog-tsk-0142`. Its implementation commit is
`ad805584c57814e959c4d41b2914133b447d311e`, already published on the matching
origin branch and open [PR #6](https://github.com/kanohorizonia/kano-git-master-skill/pull/6).
The tracked index/worktree were clean before this checkpoint; pre-existing
untracked `.omo/` review artifacts were left untouched.

Intent preflight: continue the current bounded same-file preservation route.
The item has no parent. Reuse its reviewed native implementation and existing
functional cases. Preserve active writer ownership, original staged/worktree
versions, real lock/ref/identity gates, and publication state distinctions.
Do not repeat implementation or apply recovery to live A1 code/Blueprints.

This checkpoint adds evidence only. It does not modify native code, tests,
installed files, canonical backlog, live RPG work, or approved Blueprints.

## Independent executable proof

The committed-head native Debug binary executed in a retained disposable
synthetic repository under
`.kano/tmp/kog0142-independent-20260930/overlap-proof`. Provenance is
`codex/disposable-synthetic-owner`; `--owner-stable` applies only to this
operator-owned fixture. The baseline and preserved content contain no secrets.

| Artifact | Readback |
| --- | --- |
| Baseline | `e858385f41dde295f4bdc9114cef2d39f4f3748c` |
| Checkpoint ID | `kog0142-independent-20260930` |
| Separate local ref | `refs/kog/checkpoints/kog0142-independent-20260930` |
| Working checkpoint commit | `5a267f0b98e71c45a909f2de90236492af6bea0c` |
| Own-only fixture commit | `a17bafff6ad9f6a4513945f50141df6e0a5ca1ea` |

The capture receipt returned `saved` and `restoreVerified=true`. Capture left
the selected index entry and working bytes intact. The supported own-change
command returned `committed`, `checkpointStatus=saved`,
`unrelatedStagedPreserved=true`, and `integrated/published/consumed=false`:

```text
kano-git agent-queue checkpoint capture --id kog0142-independent-20260930 --path shared.txt --source codex/disposable-synthetic-owner --work-item KOG-TSK-0142 --owner-stable
kano-git commit --exact-path shared.txt --overlap-checkpoint kog0142-independent-20260930 --expected-head e858385f41dde295f4bdc9114cef2d39f4f3748c -m "[Test][Chore] Commit independent own change (KOG-TSK-0142)"
```

Readback verified the original working version in the checkpoint commit and
the original staged version in its parent. The caller commit contains only its
own final-line change against the original baseline. The checkpoint is not an
ancestor of the caller commit. The selected staged intent is rebased exactly
and the final working intent remains exact.

All ten unrelated files retain their original staged bytes, index OIDs/modes,
working bytes, and `AM` status. `unrelated-0.txt` retains mode `100755`; the
other nine retain `100644`. Original synthetic versions, receipts, replay, and
readback verifier remain under the ignored run directory. The durable
[structured readback](kog-tsk-0142-independent-checkpoint-20260930.json)
records each file separately.

One initial invocation added `--agent codex` to this exact-path route. The CLI
rejected the unsupported option combination with exit 2 before commit; HEAD
and all fixture changes remained intact. The supported invocation above then
passed. No reset, stash, clean, lock deletion, or fixture recreation was used.

## Validation

Existing focused binaries ran inside the repository Pixi environment:

```text
pixi run --manifest-path pixi.toml .\src\cpp\out\bin\windows-ninja-msvc\debug\kano_git_cli_tests.exe [KOG-TSK-0142] --reporter compact
pixi run --manifest-path pixi.toml .\src\cpp\out\bin\windows-ninja-msvc\debug\kano_git_cli_tests.exe [KG-TSK-0110] --reporter compact
pixi run --manifest-path pixi.toml .\src\cpp\out\bin\windows-ninja-msvc\debug\kano_git_cli_tests.exe [KG-TSK-0112] --reporter compact
pixi run --manifest-path pixi.toml .\src\cpp\out\bin\windows-ninja-msvc\debug\kano-git.exe regression coverage --fail-on-gap
pixi run --manifest-path pixi.toml python .kano/tmp/kog0142-independent-20260930/verify_readback.py
```

| Validation | Result |
| --- | --- |
| `[KOG-TSK-0142]` | Exit 0; 344 assertions / 4 cases |
| `[KG-TSK-0110]` | Exit 0; 70 assertions / 4 cases |
| `[KG-TSK-0112]` | Exit 0; 97 assertions / 3 cases |
| Registry gap gate | Exit 0; 36 incidents / 0 gaps |
| Independent retained-fixture readback | Exit 0; exact preservation and own-only tree |
| Readback JSON syntax / `repo-hygiene check` | Exit 0; valid JSON / no hygiene issues |
| `git diff --check` | Exit 0 |

The four focused cases execute preservation, inseparable/binary refusal,
secret refusal, active KOG mutation/index locks, HEAD drift, remote divergence,
and hook rejection/mutation. Source mapping counts are not substituted for
execution. New logs are retained locally in the ignored run directory; private
environment diagnostics are not copied into this report.

## Consumption and remaining boundary

The installed shim resolves to `C:/Users/dorgon.chang/.kano/bin/kog.ps1` and
reports `0.0.1.1024`. Its `agent-queue` help lacks `checkpoint`; it has not
consumed this feature. The source native command and source skill contract
agree on local checkpoint preservation. Installed runtime/skill consumption,
default-branch convergence, release, and live A1 dogfood remain unverified.
The existing implementation PR being published does not publish fixture WIP
refs or prove installed consumption.

The existing PR release quality gate and all three TUI focus jobs failed
before test execution. Structured GitHub check annotations report Git exit 128:
`No url found for submodule path 'assets/ignore-sources/upstream/github-gitignore' in .gitmodules`.
Both the implementation and its unchanged base `d2dc850b` have gitlink
`assets/ignore-sources/upstream/github-gitignore` at
`dcc0fc7bc2b5ba480cf117ad1be31bafceeaff46`, while `.gitmodules` registers
`assets/ignore/datasource/upstream/github-gitignore`. The implementation did not
change either path. Native/coverage/MSI jobs were skipped by that checkout
failure. Main integration still requires this repository checkout gate to be
resolved through its existing owner; this evidence checkpoint leaves
submodules unchanged.

Full remote failure-log copying was rejected by automatic approval review
because unknown log contents may include credentials/private diagnostics. No
raw remote logs were copied. Bounded public check annotations established the
concrete failure above without requiring a full-log audit.

`--owner-stable` is caller asserted. KOG enforces its cooperating writer locks,
active batch, index lock, branch/HEAD, and snapshot drift gates; it cannot
independently establish the stability of an editor outside those gates.
Windows tests passed; Linux/macOS execution and selected mode/index-flag or
atomic index replacement failure injection were not run in this checkpoint.

Do Not compliance: bounded evidence only; no duplicate infra/task, no live
writer takeover, no unrelated staging mutation, no secret preservation, and no
premature `Done` claim. These KOG-specific remaining gates do not block a
separately verified KOB/KOA local allocation/readback/source-admission route.
