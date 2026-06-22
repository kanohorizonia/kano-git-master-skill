# Public Artifact Contract

This contract defines what `kano-git-master-skill` may claim for public release artifacts. It separates source tags, local archive exports, package-manager recipe scaffolding, and future downloadable binaries.

## Release Identity

- Source tag: `v<version>`, for example `v0.0.1`.
- Version source: root `VERSION`.
- Current public baseline: `0.0.1` alpha.
- A public release claim must name the exact git commit and tag.

## Artifact Classes

### Source Archive

Source archives are produced from the repository content at a tag.

Expected command:

```bash
./scripts/kog export --single --validate-release-archive
```

Required outputs:

- root `.tar` archive;
- export manifest;
- optional `.sha256` sidecar when requested;
- offline smoke validation result.

Boundary:

- This archive is the current public artifact class.
- It must not require private remotes or machine-local paths.

### Native Binary Payload

Native binary payloads are planned public artifacts, not automatically published by the docs alone.

Expected sources:

- Windows MSVC release build from `./scripts/kog self build`;
- future Linux/macOS release jobs from the shared native CI lanes.

Required outputs before public download claims:

- platform-qualified binary or installer name;
- versioned directory or filename containing the release version;
- SHA-256 checksum;
- provenance that names commit, tag, host platform, toolchain, and build preset;
- smoke result showing the binary can print help and version.

Naming pattern:

```text
KanoGit-<version>-<platform>-<arch>.<ext>
```

Examples:

```text
KanoGit-0.0.1-windows-x64.msi
kano-git-0.0.1-linux-x64.tar.gz
kano-git-0.0.1-macos-arm64.tar.gz
```

### Package Manager Recipes

Recipe generation is metadata scaffolding unless the target package channel has accepted and published the recipe.

Current scripts:

- `src/shell/release/02-generate-winget-skeleton.sh`
- `src/shell/release/02-generate-winget-from-artifacts.sh`
- `src/shell/release/03-generate-homebrew-skeleton.sh`

Required inputs:

- repository root;
- tag name;
- repo slug;
- artifact directory containing the referenced binary/archive;
- output directory for generated recipe files.

Required outputs:

- generated recipe/manifests;
- resolved download URL;
- SHA-256 checksum of the referenced artifact.

Boundary:

- Winget/Homebrew outputs are preview metadata until submitted to and accepted by the public package channel.
- Internal publish scripts must not be described as public channel publication.

### Internal Publish Lanes

These scripts are internal-only:

- `src/shell/release/04-publish-winget-internal.sh`
- `src/shell/release/05-publish-homebrew-internal.sh`

They fail closed unless their internal repository/tap environment variables are supplied. Public release notes must not treat these scripts as evidence that public Winget/Homebrew packages are live.

## Manifest And Checksum Requirements

Every public artifact record should include:

- artifact filename;
- version;
- git commit;
- git tag;
- platform and architecture;
- build preset or export mode;
- SHA-256 checksum;
- creation command;
- validation command and result;
- public or private visibility.

If an upload helper copies to a local sync folder, the artifact is not public until the operator publishes or shares it through the target service.

## Dry-Run Behavior

Release helpers should support preview or fail-closed behavior before mutation:

- recipe generation may write to a local output directory;
- internal publish scripts require explicit environment configuration;
- public sharing links require explicit operator confirmation;
- no script should infer public availability from local file existence alone.

## Validation Checklist

Before claiming a public artifact:

```bash
git status --short --branch
git tag --list v<version>
./scripts/kog version
./scripts/kog --help
./scripts/kog export --single --validate-release-archive
src/shell/test/smoke-release-archive.sh <archive.tar>
```

For package recipes:

```bash
src/shell/release/02-generate-winget-from-artifacts.sh <repo-root> <artifact-root> <tag> <repo-slug>
src/shell/release/03-generate-homebrew-skeleton.sh <repo-root> <tag> <repo-slug> <artifact-dir> <output-dir>
```

The command output must record the artifact path and checksum used by the generated metadata.
