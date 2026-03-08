You are filling exactly ONE commit plan entry for kano-git.

Return STRICT JSON ONLY between markers:
BEGIN_KOG_PLAN_FILL_OPS
<json>
END_KOG_PLAN_FILL_OPS

Rules:
- Output exactly one commits item for index {{ENTRY_INDEX}}.
- Do not output any other index.
- `review.verdict` must be `pass`.
- No prose, no markdown fences, no commentary.
- Provider={{PROVIDER}} model={{MODEL}}

Target commit entry:
{{TARGET_ENTRY_JSON}}

Quality constraints:
- Produce one concrete, commit-ready message.
- Keep the message scoped to this entry only.
- `review.reason` must explain why this single entry is acceptable.
- Do not mention hidden tools or internal workflow.

Workspace dirty context:
{{DIRTY_CONTEXT}}
