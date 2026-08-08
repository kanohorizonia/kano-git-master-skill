# KOG Audit Contract v1

KOG is an agent-first Git execution layer normally driven by KOA. Its audit
contract records what KOG can prove about one run without duplicating KOA's
dispatch, queue, approval, or policy authority.

The native `kano::git_audit` facade is the authoritative parser, validator, and
canonical serializer. It is a leaf of `kano_git_core`, so commit-plan,
converge, journal, provider, and read-only TUI consumers can share the same
types without private cross-module includes.

## Published contracts

| Contract | Discriminator | Published schema |
|---|---|---|
| Closed producer capability | `schemaName=kog.auditCapability`, `schemaVersion=1` | `assets/audit/schemas/kog.auditCapability.v1.schema.json` |
| One semantic action event | `schemaName=kog.auditEvent`, `schemaVersion=1` | `assets/audit/schemas/kog.auditEvent.v1.schema.json` |
| One terminal run receipt | `schemaName=kog.runReceipt`, `schemaVersion=1` | `assets/audit/schemas/kog.runReceipt.v1.schema.json` |
| Path-free verification result | `schemaName=kog.auditVerification`, `schemaVersion=1` | `assets/audit/schemas/kog.auditVerification.v1.schema.json` |

The combined names `kog.auditEvent.v1` and `kog.runReceipt.v1` identify the
frozen v1 contracts. Name and numeric version are separate JSON fields so
version negotiation remains explicit.

## Identity and correlation

Every event and receipt carries the same:

- `runId`, required nullable `parentRunId`, and positive `attempt`;
- immutable `planId` and lowercase SHA-256 `planSha256`;
- strict UTC timing;
- explicit `correlation`.

`runId` and `attempt` are the canonical identities shared with KOA. They are
not duplicated inside the correlation envelope.

Correlation has two modes:

- `standalone`: every KOA-specific identifier is explicitly `null`;
- `koa`: product, item, work order, request, producer, and route IDs are
  required. Topic and agent IDs remain explicit nullable fields.

Correlation is provenance, never authorization. Policy and approval decisions
remain opaque hash-bound references.

Stable correlation, run, event, plan, artifact, policy, and approval IDs are
1..128 ASCII bytes. The first byte is alphanumeric; every remaining byte is
`[A-Za-z0-9._:@-]`. `..` is forbidden, as are secret-like prefixes.
The case-insensitive rejected prefixes are `sk-`, `ghp_`, `github_pat_`,
`glpat-`, `bearer`, and `akia`. MIME content types use separately bounded
printable ASCII. Human-facing branch names remain valid bounded Unicode.

## Events

An AuditEvent is one completed semantic KOG action. It contains:

- positive contiguous `sequence`, starting at one;
- bounded `phase` and `action` tokens such as `mutation` and `commit.apply`;
- one logical repository ID and typed before/after state;
- one typed outcome;
- bounded policy, approval, and artifact reference arrays.

`action` is semantic data. It must never contain argv, a shell command, or raw
console text.

Repository state distinguishes `clean`, `dirty`, and `unknown`; absent facts
are never interpreted as clean. Known worktree state requires a SHA-256 dirty
fingerprint. Head and branch are nullable for unborn or detached repositories.
Upstream head, ahead, and behind are either all present or all null.

## Terminal receipt

A RunReceipt is the single terminal truth source for a run. It binds:

- terminal outcome and enclosing start/finish timestamps;
- first/last sequence and exact event count;
- SHA-256 of the canonical JSONL event bytes;
- per-repository aggregate before/after state;
- bounded policy, approval, and artifact references.

A zero-event receipt is valid only for explicit crash-before-first-event
recovery with terminal outcome `unknown`. Missing terminal state is invalid.

When validating a receipt with its event stream, KOG rejects identity drift,
duplicate event IDs, gaps, duplicate or non-monotonic sequence, broken
repository transition chains, invented or omitted repositories, count/range
mismatch, events outside run time, evidence equivocation, and a successful
receipt containing any non-success event. Receipt policy, approval, and
artifact arrays are the exact canonical union of a non-empty event stream.

A zero-event crash receipt binds the SHA-256 of empty bytes and cannot claim a
repository transition.

## Outcome truth table

The only outcome authority is `status`:

| Status | Successful | Required `reasonCode` | Exit-code rule |
|---|---:|---:|---|
| `succeeded` | yes | no; must be `null` | `null` or `0` |
| `failed` | no | yes | `null` or non-zero |
| `partial` | no | yes | `null` or non-zero |
| `blocked` | no | yes | normally `null`; never `0` |
| `cancelled` | no | yes | normally `null`; never `0` |
| `timed-out` | no | yes | `null` or non-zero |
| `unknown` | no | yes | `null` or non-zero |

`succeeded` also requires `retryable=false`. There is no separate `ok`,
`success`, or `failed` boolean that can contradict the status.

## Evidence and redaction boundary

Artifacts contain only:

- opaque ID and semantic kind;
- SHA-256 and byte size;
- content type;
- redaction status: `not-required`, `redacted`, or `withheld`.

There are no artifact bodies, paths, commands, argv, environment, stdout,
stderr, secrets, or free-form error messages in v1. Hash and size identify the
persisted post-redaction bytes. A withheld artifact refers to the persisted
withholding tombstone, not to unretained sensitive bytes.

The schema bounds references, strings, repositories, events, and all JSON
integer fields. The native parser additionally caps one JSON document at
4 MiB and one JSONL stream at 64 MiB. Integers do not exceed JavaScript's
exact `2^53-1` range; `attempt` is further bounded to an unsigned 32-bit value.

## Canonical JSON and JSONL

Canonical serialization is:

- UTF-8 without BOM;
- compact JSON with lexicographically ordered object keys;
- no floats;
- every nullable contract field emitted explicitly as `null`;
- policy/approval references and artifacts sorted by ID;
- receipt repositories sorted by logical repository ID.

JSON serialization adds no newline. JSONL is exactly one canonical AuditEvent
per line with one LF after every record, including the last. CRLF, blank
records, multiline records, and a missing final LF are invalid.

## Compatibility boundary

Parsers reject an unknown schema name, version, enum, field, or
safety-driving value. Every v1 object, including the document root, is closed.
Producers that need additional data must publish a new schema version instead
of relying on a reader to silently discard unauthenticated metadata.

Raw or conflicting fields such as `command`, `argv`, `environment`, `stdout`,
`stderr`, `secret`, `body`, `payload`, `path`, `ok`, and `success` are outside
the closed contract. Incompatible changes require a new schema version; v1
includes no migration layer.

The schemas describe one JSON object. Stream framing, real-calendar timestamp
validation, cross-event ordering, and receipt/event reconciliation remain
native-validator responsibilities.

## Scope boundary

The core `kano::git_audit` facade defines and validates evidence objects; it
does not populate KOA context, execute Git, authorize approvals, or render the
TUI. KG-TSK-0125's command runtime does persist the current attempt's frozen
input, events, terminal receipt, and incomplete marker. Broader journal
retention, indexing, recovery, and lifecycle policy remain the KG-TSK-0126
slice. A timestamp, event, or receipt is evidence only when the validator
accepts the complete trace and the referenced hashes match persisted bytes.

## Closed producer capability

`kog audit capability --json` returns the closed, path-free capability object.
The v1 route/input matrix is exact:

| Route | Frozen input kind |
|---|---|
| `commit.plan` | `commit-plan` |
| `commit-push.plan` | `commit-plan` |
| `plan.apply` | `commit-plan` |
| `converge.repos` | `operation-descriptor` |
| `converge.branches.apply` | `operation-descriptor` |
| `converge.branches.recover` | `operation-descriptor` |
| `converge.branches.retire` | `operation-descriptor` |

Every listed route reserves before its first mutation, retains one active
owner, appends ordered internal per-repository mutation events, and publishes
one terminal receipt. Nested plan execution borrows the active owner and may
add a hash-bound supplemental frozen-plan artifact; it never publishes a
nested receipt. A route/input mismatch is rejected before reservation.

Each converge route freezes a closed, typed operation descriptor. Descriptor
options contain only bounded booleans, integer job counts, closed strategy
enums, required commit SHA-1 evidence, nullable SHA-256 selector digests, and
the correlation envelope. Raw remotes, URLs, filesystem paths, branches,
targets, commit messages, and review text are never persisted in the frozen
descriptor. Selector digests are evidence bindings, not reversible locators or
authority.

Mutation selectors are admitted before correlation-file loading, audit
reservation, workflow-state writes, or Git mutation. Branch selectors are
bounded, reject control, option-like, absolute, traversal, and Windows-path
shapes, and must round-trip through `git check-ref-format --branch`. Remote
selectors use a closed logical-name grammar of ASCII alphanumerics plus `.`,
`_`, and `-`; leading `-`, `.`/`..`, paths, URLs, controls, and overlong names
are rejected.

Legacy synthetic `kog commit -m` is not advertised as `commit.plan`.
`amend.plan` is also outside the closed set: an amend request carrying
`mode=koa` is rejected before provider bootstrap, staging, or Git mutation.
Provenance does not grant authority (`provenanceGrantsAuthority=false`).

`kog audit verify --plan-file P --run-id R --attempt N --json` resolves the
private evidence location from the plan identity and returns no locator path.
A successful closed result binds the exact frozen bytes and their SHA-256,
plan/event-stream hashes, `receiptSha256`, run/parent/attempt correlation,
terminal outcome, typed repository transitions, and hash-bound artifacts.
The currently admitted plan bytes must exactly match either the initial source
artifact or an explicitly frozen post-stamp source-state artifact; verification
does not normalize, reconstruct, or heuristically ignore changed fields. The
admitted plan ID must also equal the receipt plan ID.
Missing, malformed, contradictory, tampered, overflowed, or incomplete
evidence produces a closed failure result and non-zero exit.

## Commit-plan correlation handoff

`kog plan new --correlation-file <closed-envelope.json>` snapshots explicit
standalone or KOA provenance in `meta.correlation`. KOA mode requires product,
item, work-order, request, run, producer, route, and a positive attempt; topic
and parent run are nullable. Standalone mode requires every identity field,
including topic and parent run, to be `null`. The envelope is a closed object:
unknown, missing, duplicate, wrongly typed, non-printable, overlong, or
contradictory fields are rejected before execution. A parent run cannot equal
its child run.

The correlation file reader is bounded to 64 KiB and accepts only a stable
regular file opened without following links; identity, size, and timestamps
must remain stable across the read. `plan prepare` and `plan verify` preserve
an existing envelope byte-for-semantic-value. A legacy plan with no
`meta.correlation` remains compatible: execution treats it as unbound
standalone and materializes the closed standalone envelope plus newly generated
run ID only in `frozen-plan.json`. It never injects that generated run into the
caller's source plan.

Correlation identifiers use the new 128-byte stable-ID grammar. This does not
narrow the existing v1 audit ABI: non-correlation opaque IDs in v1 events and
receipts retain their original 256-byte printable-ASCII contract.

Plan-backed execution snapshots and validates the exact source bytes, then
reserves this immutable attempt directory before creating the persistent plan
execution lock or performing any Git mutation. Once the lock is held, KOG
immediately performs a second bounded no-follow read and compares both the
SHA-256 and exact admitted bytes before any later action. Lock acquisition and
failure are recorded as audit events. Malformed correlation therefore creates
neither the execution-lock path nor plan/Git state. A stale shared agent plan
fails closed with a separate preparation instruction; audited execution never
refreshes or rewrites it. For a plan
inside a Git worktree, evidence is stored below the absolute Git directory so
the writer cannot perturb the worktree it is measuring:

`<git-dir>/kog/audit/plan-<sha256(canonicalPlanPath)>/run-<sha256(runId)>/attempt-<attempt>/`

For a non-Git workspace (including contract tests), the fallback is
`<plan>.audit/run-<sha256(runId)>/attempt-<attempt>/`.

The directory contains `frozen-plan.json`, and every plan-driven safety or
mutation phase reads that snapshot. Source and frozen SHA-256 values are bound
separately because standalone run materialization can intentionally make their
bytes differ. The original plan path is used only for ownership-checked stage
stamps; compare-and-swap uses the captured source bytes (or the explicitly
owned updated source), never the derived frozen bytes. Failure cleanup changes
only a stamp still owned by the same run.

Schema-v3 source-plan stamping remains a mutable compatibility path. After the
exclusive audit attempt is reserved, the persistent execution lock serializes
cooperating KOG writers from admission recheck through Git mutation, source
stamp, and terminal verification. Windows additionally holds an exclusive
no-follow source handle while conditionally rewriting the plan. On POSIX,
source mutation and exact owned-stamp restoration require every plan writer to
cooperate with the advisory `flock`; bounded byte, identity, metadata, and path
checks fail closed when they observe drift, but do not claim a linearizable
compare-and-swap against a hostile writer that ignores that lock.

For a Git worktree, persistent plan locks live under the absolute common Git
metadata directory at `<git-common-dir>/kog/plan-execution-locks/`. They never
appear below the consumer worktree or affect raw dirty-state admission. Lock
files remain in place after unlock so concurrent processes cannot split
exclusion across old and newly created inodes. Non-Git execution retains the
workspace-local `.kano/tmp/git/plan-execution-locks/` fallback. Both locations
use the same anchored traversal: each directory component is opened without
following links before the next component or lock file is created.
The lock name is derived from the opened plan file identity (device/inode on
POSIX; volume/file index on Windows), not from caller-controlled path spelling,
so hard-link and case aliases converge on the same lock. POSIX creation and file
open remain anchored to directory handles, while Windows pins verified
non-reparse directory handles until the regular lock handle is open.

After admission and safety checks, each `commit-push.plan` attempt
conditionally clears any prior `meta.executed_at_utc` value and records
`plan.stamp.clear` before its first Git mutation. The exact cleared bytes become
the attempt-owned source state for later compare-and-swap and rollback. A
completion timestamp is published only after push succeeds. A clear that cannot
be audited is conditionally rolled back to the admitted bytes; after a
successful audited clear, any later failed attempt remains uncompleted instead
of reviving the prior timestamp.

The raw run ID is retained in every event and receipt but is never used as a
filesystem segment. KOA reservation is exclusive, so a repeated explicit
run/attempt cannot overwrite earlier evidence; a retry of the same KOA run must
carry the next explicit attempt. Each independent standalone execution creates
a fresh internal run ID, so two ordinary standalone invocations do not collide.
`events.jsonl` is append-only and contains observed reservation, safety, commit,
sync, post-sync, push, and plan-stamp actions as those actions actually run.
Each append is flushed to stable storage. `receipt.json` is written through an
exclusively reserved temporary handle, atomically published without replacement,
reopened and file-flushed, and directory-synced only after the event stream is
durable. Reservation first creates and flushes `publication-pending.json` and
syncs its attempt directory. Verification rejects the attempt while that
sentinel exists, including the interval where `receipt.json` is already visible.
Only after the receipt file and its directory entry are durable does KOG remove
the sentinel and sync the attempt directory again. Every newly created audit
hierarchy directory entry also syncs its parent. On POSIX each directory sync
is required. On Windows, KOG first verifies a non-reparse directory handle,
then attempts a separate directory flush; only documented unsupported-directory-
flush errors are best effort, while file flushes remain required.

`receipt.json` alone is not sufficient terminal proof. A failure before
publication writes `incomplete.json`; a failure after the receipt became
visible but before its directory durability was proven writes an incomplete
marker with `receiptPublished=true` and
`reasonCode=receipt-durability-uncertain`. Verification checks the marker both
before and after reading evidence and fails closed whenever it is present.
Verification likewise checks the publication-pending sentinel before and after
the evidence reads. A
post-reservation execution exit therefore yields either a truthful terminal
receipt or explicit incomplete evidence.

Every phase record and the terminal receipt retain the frozen plan hash, KOA
identity chain, run/parent linkage, and attempt. Writer preflight or publication
failure is fail-closed. A failed execution never publishes a new completion
stamp; once its audited pre-mutation clear succeeds, the source plan remains
uncompleted. Correlation remains provenance only and never grants authority.

Git-reported path collections use NUL framing throughout scoped commit and
secret-gate discovery. Tabs and newlines in filenames remain data and cannot
become record separators.
