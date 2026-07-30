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
| One semantic action event | `schemaName=kog.auditEvent`, `schemaVersion=1` | `assets/audit/schemas/kog.auditEvent.v1.schema.json` |
| One terminal run receipt | `schemaName=kog.runReceipt`, `schemaVersion=1` | `assets/audit/schemas/kog.runReceipt.v1.schema.json` |

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

Opaque identifiers and MIME content types use bounded printable ASCII.
Human-facing branch names remain valid bounded Unicode.

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

This contract does not persist journals, populate KOA context, execute Git,
authorize approvals, or render the TUI. Those are later consumers of this
facade. A timestamp, event, or receipt is evidence only when the validator
accepts the complete contract and the referenced hashes match persisted data.
