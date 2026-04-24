# Testing Guide

`TESTING.md` is kept only as a compatibility entrypoint for older links.

Canonical test documentation now lives in:

- `docs/development/testing.md`

Recommended shared-infra flow for native tasks:

```bash
pixi install --manifest-path src/cpp/shared/infra/pixi.toml
pixi run --manifest-path src/cpp/shared/infra/pixi.toml quick-test
pixi run --manifest-path src/cpp/shared/infra/pixi.toml full-test
```
