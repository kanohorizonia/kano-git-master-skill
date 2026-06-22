# KCC Commit Message Policy

Kano Commit Convention (KCC) gives agent and human commits a stable subject shape for backlog traceability and reporting.

## Subject Shape

Use:

```text
[Area][Intent] Concise subject (TICKET-ID)
```

Examples:

```text
[Docs][Feature] Revamp README landing page (KOB-TSK-0042)
[KOG][BugFix] Keep plan-file commits intent-scoped (KOA-TSK-0149)
[Backlog][Task] Link public artifact contract (KG-FTR-0014)
```

Area examples:

- `KOG`
- `Backlog`
- `Docs`
- `Build`
- `Submodule`
- `Release`

Intent examples:

- `Feature`
- `BugFix`
- `Task`
- `Docs`
- `Chore`
- `Maintenance`

Ticket IDs should use the backlog identifier that owns the work, such as `KOB-TSK-0042`, `KOA-TSK-0149`, or `KG-FTR-0014`.

## NO-TICKET Exceptions

`NO-TICKET` is allowed only for explicit maintenance exceptions. Prefer opening or linking a backlog item when the work is user-visible, risky, or not obviously mechanical.

Allowed examples:

```text
[Submodule][Chore] Update shared dependency pointers (NO-TICKET)
[Build][Maintenance] Refresh generated wrappers (NO-TICKET)
```

Avoid using `NO-TICKET` for feature work:

```text
[Docs][Feature] Refresh current command docs (NO-TICKET)
```

The audit classifies that as `no-ticket-non-exception`.

## Non-KCC Ticket Prefixes

Ticket-prefix subjects keep traceability but are not KCC-compliant:

```text
KOB-TSK-0042: revamp README landing page
```

Use the bracketed KCC shape instead:

```text
[Docs][Feature] Revamp README landing page (KOB-TSK-0042)
```

## Audit Gate

Run the classifier against recent commits:

```bash
src/shell/test/audit-kcc-commit-messages.sh --max-count 50
```

Run a bounded range:

```bash
src/shell/test/audit-kcc-commit-messages.sh --range origin/main..HEAD
```

Fail when non-compliant subjects are found:

```bash
src/shell/test/audit-kcc-commit-messages.sh --range origin/main..HEAD --strict
```

Run built-in classifier coverage:

```bash
src/shell/test/audit-kcc-commit-messages.sh --self-test
```

The local pre-commit quality gate keeps this audit opt-in so legacy history does not block unrelated work:

```bash
KOG_ENABLE_KCC_AUDIT=1 src/shell/test/pre-commit-quality-gate.sh
KOG_ENABLE_KCC_AUDIT=1 KOG_KCC_AUDIT_STRICT=1 src/shell/test/pre-commit-quality-gate.sh
```

## Agent Prompt Guidance

Codex and OpenCode prompts should ask for KCC subjects explicitly:

```text
Use Kano Commit Convention: [Area][Intent] Concise subject (TICKET-ID).
Use NO-TICKET only for explicit chore, submodule, or maintenance exceptions.
Do not use ticket-prefix subjects such as KOB-TSK-0042: ...
```
