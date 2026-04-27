You are generating a complete plan JSON for kano-git.
Return STRICT JSON ONLY between markers:
BEGIN_KOG_PLAN_JSON
<json>
END_KOG_PLAN_JSON

Rules:
- Output a full JSON object with keys: meta, stages.
- Treat `stages.commit` structure as canonical. Do not replace it with alternative schemas (`id`, `files`, `commit_message`, `target_repo_path`, etc.).
- Keep existing meta fields that are already concrete values (plan_id/generated_at_utc/base_head_sha/dirty_fingerprint).
- meta.planner.provider must be "{{PROVIDER}}".
- meta.planner.ai-model must be "{{MODEL}}".
- meta.review.verdict must be "pass".
- meta.review.reason must be a concise overall review summary.
- stages.ignore must be present; keep candidate rules deterministic and based on current file/folder signals.
- Keep `meta.ignore_datasource.root` and `meta.ignore_datasource.manifest` unchanged unless paths are invalid.
- stages.commit must contain dirty repos only.
- For each repo entry, provide commits array.
- Every commit item must include:
  - message: MUST follow Kano Commit Convention (KCC) format `[Subsystem][Type] Summary`. MUST NOT include any Ticket ID or `(NO-TICKET)` suffix.
  - review.verdict = "pass"
  - review.reason = concise reason
- If commit stage is already seeded, keep repo grouping and only fill/adjust commit message + review fields.
- stages.post_sync must be [].
- Do NOT include placeholder values.

CLI building blocks (for operator/agent workflow):
- `kog plan new` creates template.
- `kog plan commit-seed` pre-populates `stages.commit` skeleton from dirty repos.
- `kog plan prepare add-commit-entry --repo "<path>" --commit.message "<msg>" --commit.include "<pathspec>" --commit.review.verdict pass --commit.review.reason "<reason>"` appends commit entries deterministically.
- `kog plan ignore-init` pre-populates ignore stage.
- `kog plan ensure-prepare-ready` fills and validates AI-ready fields.

Current template JSON (fill it, do not remove required keys):
{{TEMPLATE_JSON}}

Workspace dirty context:
{{DIRTY_CONTEXT}}
