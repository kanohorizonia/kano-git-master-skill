# Regression registry assets

- `incidents.json` is the shipped dogfood incident-to-test registry consumed by
  `kog regression coverage`.
- `case-template.json` is an intentionally invalid drafting template. Replace
  all sentinel values before merging a case into the registry.

The schema, naming rules, workflow markers, and exit-code contract are defined
in `docs/guides/dogfood-regression-policy.md`. Registry mappings are source
links only; they do not claim tests were executed or passed.
