# C++ coverage and PGO provider model

## Problem statement

In build scripts, the word "profile" has been overloaded to mean at least three different things:

- code coverage reports
- compiler training data used by PGO
- detached collection sessions

Treating these as one generic artifact creates invalid pipelines. Coverage reports and PGO training data are different artifacts with different consumers and must not be mixed.

## Non-goals

This model does not implement:

- real-user telemetry profile collection
- remote coverage server architecture
- GitHub Actions workflow redesign
- Jenkins pipeline redesign
- removal of existing providers

## Tool model

- Microsoft.CodeCoverage.Console
  - native/static coverage instrumentation and collection lane
  - supports process-wrapper and local detached session concepts
- OpenCppCoverage
  - process-wrapper coverage collector for native binaries
  - can wrap MSVC PGO training executions while keeping artifacts separate
- MSVC PGO
  - training data via `/GENPROFILE` or `/FASTGENPROFILE`
  - optimized build consumption via `/USEPROFILE`
- LLVM coverage + LLVM PGO
  - source-based instrumentation can support unified profile-data workflows
- Microsoft.CodeCoverage.Console server-mode
  - local/session detached collection only
  - not remote telemetry

## Terminology

- unifiedExecution: one executable run can emit both coverage and PGO-related outputs
- unifiedProfileData: one underlying instrumentation/profile data source can be consumed by both coverage and PGO tools
- splitLanes: coverage and PGO are separate lanes (build/run/report lifecycle separated)
- coverageProvider: `opencppcoverage | microsoft-codecoverage | llvm-cov | none`
- pgoProvider: `msvc-pgo | llvm-profdata | none`
- coverageSubject: `normal-test-binary | pgo-instrumented-training-binary | instrumented-coverage-binary | llvm-instrumented-binary`
- collectorScope: `process-wrapper | local-session-server | none`
- remoteTelemetry: whether the collector transports production user telemetry
- realUserProfile: whether data comes from real user telemetry/profiling

## Capability matrix

| compiler | coverageProvider | pgoProvider | unifiedExecution | unifiedProfileData | coverageSubject | collectorScope | remoteTelemetry | realUserProfile | recommendedUse |
|---|---|---|---|---|---|---|---|---|---|
| msvc | opencppcoverage | msvc-pgo | true | false | pgo-instrumented-training-binary | process-wrapper | false | false | optional `pgo-gather-with-coverage` training observation lane |
| msvc | microsoft-codecoverage | msvc-pgo | false | false | instrumented-coverage-binary | process-wrapper | false | false | split lanes only (`coverage-all` and `pgo-*` separately) |
| msvc | microsoft-codecoverage (server-mode) | none | false | false | instrumented-coverage-binary | local-session-server | false | false | local detached coverage collection |
| clang | llvm-cov | llvm-profdata | true | true | llvm-instrumented-binary | process-wrapper | false | false | unified LLVM profile-data lane |
| any | opencppcoverage\|microsoft-codecoverage\|llvm-cov | none | false | false | normal/instrumented coverage target | process-wrapper/local-session-server | false | false | canonical `coverage-all` split lane |
| any | none | msvc-pgo\|llvm-profdata\|none | false | false | normal-test-binary | none | false | false | canonical `pgo-gather` / `pgo-rebuild` split lane |

## Proposed KOG task model

- coverage-all
  - canonical coverage lane (build -> gather -> report)
- pgo-gather
  - canonical PGO training lane (collect/training data only)
- pgo-rebuild
  - canonical release PGO lane (gather + optimized rebuild)
- pgo-gather-with-coverage
  - optional lane for supported unified backends
- profile-run-manifest
  - generate capability manifest for selected run mode/providers

## Profile-run manifest schema

Fields:

- schemaVersion
- profileRunMode
- compiler
- coverageProvider
- pgoProvider
- unifiedExecution
- unifiedProfileData
- splitLanes
- coverageSubject
- collectorScope
- remoteTelemetry
- realUserProfile
- pgoDataPaths
- coverageReportPaths
- trainingCommand
- coverageCommand
- notes

### Example 1: MSVC + OpenCppCoverage + MSVC PGO

```json
{
  "schemaVersion": "1.0",
  "profileRunMode": "pgo-gather-with-coverage",
  "compiler": "msvc",
  "coverageProvider": "opencppcoverage",
  "pgoProvider": "msvc-pgo",
  "unifiedExecution": true,
  "unifiedProfileData": false,
  "splitLanes": false,
  "coverageSubject": "pgo-instrumented-training-binary",
  "collectorScope": "process-wrapper",
  "remoteTelemetry": false,
  "realUserProfile": false,
  "pgoDataPaths": ["out/bin/windows-ninja-msvc-pgo-collect/debug/*.pgd"],
  "coverageReportPaths": [".kano/tmp/pgo/gather-reports/coverage/raw/*.cobertura.xml"],
  "trainingCommand": "kano pgo gather",
  "coverageCommand": "OpenCppCoverage -- ...",
  "notes": ["Coverage output remains separate from PGO training data."]
}
```

### Example 2: MSVC + Microsoft.CodeCoverage.Console server-mode

```json
{
  "schemaVersion": "1.0",
  "profileRunMode": "coverage-all",
  "compiler": "msvc",
  "coverageProvider": "microsoft-codecoverage",
  "pgoProvider": "none",
  "unifiedExecution": false,
  "unifiedProfileData": false,
  "splitLanes": true,
  "coverageSubject": "instrumented-coverage-binary",
  "collectorScope": "local-session-server",
  "remoteTelemetry": false,
  "realUserProfile": false,
  "pgoDataPaths": [],
  "coverageReportPaths": [".kano/tmp/coverage/**/*.cobertura.xml"],
  "trainingCommand": "",
  "coverageCommand": "Microsoft.CodeCoverage.Console collect --server-mode",
  "notes": ["Detached local session."]
}
```

### Example 3: Clang + LLVM unified profile data

```json
{
  "schemaVersion": "1.0",
  "profileRunMode": "pgo-gather-with-coverage",
  "compiler": "clang",
  "coverageProvider": "llvm-cov",
  "pgoProvider": "llvm-profdata",
  "unifiedExecution": true,
  "unifiedProfileData": true,
  "splitLanes": false,
  "coverageSubject": "llvm-instrumented-binary",
  "collectorScope": "process-wrapper",
  "remoteTelemetry": false,
  "realUserProfile": false,
  "pgoDataPaths": ["out/**/*.profraw"],
  "coverageReportPaths": ["out/**/*.cobertura.xml"],
  "trainingCommand": "clang pgo gather",
  "coverageCommand": "llvm-cov export/show",
  "notes": ["One data source is consumed by coverage and PGO."]
}
```

## Recommended defaults

- Windows native C++ default coverage provider should be OpenCppCoverage for pragmatic CI and PGO-training coverage observation if the tool is available.
- Microsoft.CodeCoverage.Console should be an optional official/enterprise coverage lane for Visual Studio/Azure/.coverage/merge/convert/C++ + C# mixed scenarios.
- coverage-all is canonical coverage.
- pgo-gather / pgo-rebuild are canonical release PGO.
- pgo-gather-with-coverage is optional training-run coverage.

## Guardrails

- "Microsoft.CodeCoverage.Console coverage output is not MSVC PGO training data."
- "MSVC unified PGO+coverage execution is only supported with OpenCppCoverage."
- "Microsoft.CodeCoverage.Console server-mode is local/session detached collection, not remote telemetry."

Additional guardrails:

- Never feed Microsoft coverage output into `/USEPROFILE`.
- Never describe OpenCppCoverage output as PGO data.
- Keep OpenCppCoverage, Microsoft coverage, and LLVM lanes available.

## Implementation roadmap

Implemented in this patch:

- profile capability resolver (`scripts/profiling/profile_run_capabilities.py`)
- profile-run manifest stage (`scripts/stages/profile-run-manifest.sh`)
- optional `pgo-gather-with-coverage` workflow lane with provider guardrails
- pgo workflow integration that resolves/records profile-run manifest before execution
- split-lane docs for `kog self build --pgo`

Remaining follow-ups:

- extend matrix/CI job wiring to persist manifest artifacts per build lane
- add richer report packaging links for profile manifest and lane metadata

## Validation plan

- Unit tests for capability resolver combinations:
  - MSVC + OpenCppCoverage + MSVC PGO unified lane allowed
  - MSVC + Microsoft coverage + MSVC PGO unified lane rejected with required message
  - Microsoft server-mode classified as local-session-server with telemetry flags false
  - Clang LLVM unified profile-data lane allowed
  - split lanes allow missing opposite provider
- Run `kog self build --pgo` with:
  - `KANO_CPP_INFRA_COVERAGE_TOOL=opencppcoverage`
  - `KANO_CPP_INFRA_COVERAGE_TOOL=microsoft` (split prepass path)
- Verify manifests and logs do not claim coverage output is PGO training data
