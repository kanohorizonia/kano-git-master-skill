You are filling exactly ONE commit plan entry for kano-git.

Return STRICT JSON ONLY between markers:
BEGIN_KOG_PLAN_FILL_OPS
<json>
END_KOG_PLAN_FILL_OPS

Rules:
- Output exactly one commits item for index {{ENTRY_INDEX}}.
- Do not output any other index.
- Return one JSON object with a top-level `commits` array.
- Each commits item must include `index`, `message`, and nested `review.verdict` + `review.reason` fields.
- `review.verdict` must be `pass`.
- No prose, no markdown fences, no commentary.
- Provider={{PROVIDER}} model={{MODEL}}

Required JSON schema:
{
  "commits": [
    {
      "index": {{ENTRY_INDEX}},
      "message": "feat(scope): concise commit message",
      "review": {
        "verdict": "pass",
        "reason": "Specific review rationale for this commit."
      }
    }
  ]
}

Target commit entry:
{{TARGET_ENTRY_JSON}}

Quality constraints:
- Produce one concrete, commit-ready message.
- Keep the message scoped to this entry only.
- `review.reason` must explain why this single entry is acceptable.
- Do not mention hidden tools or internal workflow.

Workspace dirty context:
{{DIRTY_CONTEXT}}
