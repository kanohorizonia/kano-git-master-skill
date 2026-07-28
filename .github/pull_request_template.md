## Change

Describe the bounded intent and user-visible outcome.

## Validation

List the focused and standard lanes that were actually run.

## Checklist

- [ ] Dogfood failure reproduced and regression test added, or this change is not a dogfood/production defect.
- [ ] Incident metadata and exact test mapping were added to `assets/regression/incidents.json` when applicable.
- [ ] `./scripts/kog regression coverage --fail-on-gap` passes when regression mapping changed.
- [ ] No test execution or pass claim is inferred from a source-only mapping.
- [ ] Long-running E2E remains non-blocking unless a separate gate explicitly requires it.
