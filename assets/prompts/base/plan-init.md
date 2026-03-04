You are generating a complete plan JSON for kano-git.
Return STRICT JSON ONLY between markers:
BEGIN_KOG_PLAN_JSON
<json>
END_KOG_PLAN_JSON

Rules:
- Output a full JSON object with keys: meta, stages.
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
  - message (Conventional Commit one-line)
  - review.verdict = "pass"
  - review.reason = concise reason
- stages.post_sync must be [].
- Do NOT include placeholder values.

Current template JSON (fill it, do not remove required keys):
{{TEMPLATE_JSON}}

Workspace dirty context:
{{DIRTY_CONTEXT}}
