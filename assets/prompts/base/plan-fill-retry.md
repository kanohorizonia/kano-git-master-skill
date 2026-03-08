{{BASE_PROMPT}}

Retry directive (mandatory):
- Previous response was rejected by validator.
{{FAILURE_CATEGORY_LINE}}{{FAILURE_DETAIL_LINE}}- Expected commits count: {{EXPECTED_COMMITS}}
{{RETURNED_COMMITS_LINE}}- Re-output a COMPLETE commits array covering ALL indexes exactly once.
- Output STRICT JSON only between BEGIN_KOG_PLAN_FILL_OPS / END_KOG_PLAN_FILL_OPS.
{{PREVIOUS_RAW_SECTION}}