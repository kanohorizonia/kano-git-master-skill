# Kano Commit Convention (KCC) — KCC-STCC Profile

## Format

```
[<Subsystem>][<Type>] <Summary> (<Ticket>)
```

Optional breaking change flag:
```
[<Subsystem>][<Type>][Breaking] <Summary> (<Ticket>)
```

## Examples

```
[UGS][BugFix] Retry tagged output parsing on proxy glitch (JIRA-1234)
[Build][Chore] Update CI bootstrap script (PIPE-88)
[UI][Feature] Add pooled compass marker widgets (JIRA-5678)
[Core][Refactor] Extract shared path resolver (TECH-204)
[Plan][BugFix] Fix UTF-8 encoding garble in LogInvocation header (NO-TICKET)
[Test][Feature] Add bug condition and preservation property tests (NO-TICKET)
```

## Field Rules

### Subsystem (required, open set)
- Length: 2–24 characters, alphanumeric only, first char must be a letter
- No spaces, underscores, dots, or hyphens
- Recommended PascalCase (e.g. `Build`, `Core`, `Plan`, `Commit`, `Test`, `Infra`)
- No registry — lint must NOT reject unknown subsystem names

### Type (required, closed set)
Allowed values: `Feature`, `BugFix`, `Refactor`, `Perf`, `Chore`, `Test`, `Docs`

### Summary (required)
- Start with an English verb (Add / Fix / Update / Remove / Refactor)
- Keep to ~50–72 characters
- Do NOT place ticket IDs in the summary

### Ticket (required)
- Single: `(JIRA-1234)`
- Multiple: `(JIRA-1234, TECH-204)`
- No ticket: `(NO-TICKET)` — must be explicit, never omit

## Validation Regex

```
^\[[A-Za-z][A-Za-z0-9]{1,23}\]\[(Feature|BugFix|Refactor|Perf|Chore|Test|Docs)\](\[(Breaking)\])? .+ \(((NO-TICKET)|([A-Z][A-Z0-9]+-\d+))(, [A-Z][A-Z0-9]+-\d+)*\)$
```

## Important

- Do NOT use Conventional Commits format (`feat:`, `fix:`, `chore:`) — use KCC `[Subsystem][Type]` format
- Every commit message MUST end with a ticket in parentheses or `(NO-TICKET)`
