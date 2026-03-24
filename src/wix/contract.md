MSI install contract: kano-git-master skill
=========================================

Scope and ownership
-------------------

This document defines the fixed filesystem and feature contract for the
per user MSI that installs the kano-git-master skill.

The MSI is **per user only**. There is no machine wide or configurable
install root. All MSI owned content lives under the calling user's
profile.

MSI fixed install root
----------------------

The MSI owns a single, fixed install root:

* `%USERPROFILE%\\.agents\\skills\\kano_installed`

The install root is not configurable. Any existing directory at this
path that looks like a developer checkout or non MSI managed content is
treated as a blocking conflict. The installer must fail fast and report
the conflict instead of trying to merge, rename, or delete content that
it does not own.

MSI features
------------

The MSI exposes two feature boundaries that WiX authoring will map to
feature elements:

* `CoreFiles`  mandatory feature for all installs
* `ShellIntegration`  optional feature for PATH only shell integration

`CoreFiles` covers the main skill payload under the fixed install root.
`ShellIntegration` only owns PATH integration for the launcher. It does
not own completion or profile hooks and it does not run launcher-side
install commands on the user's behalf.

MSI owned installed files
-------------------------

All paths below are relative to the fixed install root
`%USERPROFILE%\\.agents\\skills\\kano_installed`.

Exact file names and sub directories are defined by the packaging input
layout, but the contract is:

* `CoreFiles` owns the on disk copy of the kano git skill code,
  including any binaries, scripts, and runtime support files that are
  shipped as part of the MSI.
* `CoreFiles` owns any static configuration or metadata files that are
  required for the skill to run and that are not user editable.
* `CoreFiles` does **not** own the install state file
  `~/.kano/git/kog-install-state.json`. That path stays owned by the
  existing installer logic.

The WiX authoring must treat every installed file under the fixed root
as MSI managed and remove it on uninstall, except where explicitly
called out as a generated/runtime artifact in the next section.

MSI owned generated and runtime files
-------------------------------------

Some files under the fixed install root may be generated at install
time or at first run, for example compiled artifacts or cached helper
data.

The contract for these files is:

* They live under the fixed install root.
* They are considered MSI owned for uninstall cleanup.
* They must not be used to store user editable configuration or
  long lived state that the user would reasonably expect to survive
  uninstall.

The MSI does **not** own any files outside the fixed install root
except for PATH updates that are explicitly part of the
`ShellIntegration` feature.

Launcher and native owned artifacts
-----------------------------------

The following artifacts are explicitly outside MSI ownership.

* The install state file
  `~/.kano/git/kog-install-state.json`. This remains at the existing
  location and is owned by the launcher/native side. The MSI must not
  create, modify, or delete this file.
* Any completion scripts, profile snippets, or shell configuration
  hooks. These are managed by the existing installer and user dotfiles
  flow, not by the MSI.
* Any developer checkouts of the kano git skill, even if placed under
  `%USERPROFILE%\\.agents\\skills`. The MSI install root is a separate
  sibling path, so normal developer checkouts do not collide by default.
  If a manual checkout exists directly at the fixed install root, the MSI
  must still treat that as external content and fail with a clear error.

Shell integration ownership model
---------------------------------

The optional `ShellIntegration` feature and the existing launcher
commands share shell related responsibilities with a strict split.

`ShellIntegration` feature (MSI owned):

* Adds a per user PATH entry that points at the packaged launcher so the
  `kog` and related commands are discoverable without extra manual
  PATH editing.
* Limits PATH changes to the current user account. No machine wide PATH
  entries or global environment blocks are authored by this feature.
* May show post install guidance that tells the user how to run
  launcher commands to set up completion or profile hooks.
* Does **not** write or modify any shell profile files such as
  `.bashrc`, `.zshrc`, or PowerShell profiles.
* Does **not** create, update, or remove completion script files.
* Does **not** call `kog completion install`, `kog install`, or
  `kano-git-installer` automatically.
* Does **not** create, update, or remove the install state file
  `~/.kano/git/kog-install-state.json`.

Launcher owned shell helpers:

* `kog completion install` and related completion commands own creating,
  updating, and removing completion files and any shell snippets needed
  to load them.
* `kano-git-installer` and any future `kog install` flow own broader
  shell setup, including profile hooks, outside this MSI.
* These helpers may be run by the user after MSI install, but they are
  not triggered by the MSI and their artifacts are not removed on MSI
  uninstall.

Native owned install state:

* Native commands that manage install state, for example `self install`
  flows in the binary, own
  `~/.kano/git/kog-install-state.json` and any related state files.
* The MSI must not create, modify, or delete install state files.

Uninstall cleanup behavior
--------------------------

On uninstall, the MSI cleanup scope is:

* Remove all MSI owned installed files under the fixed install root.
* Remove all MSI owned generated/runtime files under the fixed
  install root.
* Remove the fixed install root directory
  `%USERPROFILE%\\.agents\\skills\\kano_installed`
  if it is empty after file removal.
* For the optional `ShellIntegration` feature, revert any PATH entries
  or shortcuts that were created by this MSI.

Uninstall must **not** touch:

* `~/.kano/git/kog-install-state.json`.
* Any completion/profile hooks or user dotfiles.
* Any other directories under `%USERPROFILE%\\.agents` that are not
  part of this MSI's fixed install root.

WiX directory ids and artifact mapping
--------------------------------------

Canonical directory ids for WiX authoring are:

* `KanoUserProfile`  maps to `%USERPROFILE%` using the appropriate WiX
  per user profile resolution.
* `KanoSkillRoot`  child of `KanoUserProfile`, path
  `.agents\\skills\\kano_installed`.

All `CoreFiles` components install under `KanoSkillRoot`.

`ShellIntegration` components are limited to:

* PATH updates that reference the launcher under `KanoSkillRoot`.

No component is allowed to install outside the per user profile tree.

Build pipeline
--------------

The MSI packaging entrypoint is `src/wix/scripts/build.sh`.

Additional explicit entrypoints are also provided:

- `src/wix/scripts/build.sh` — normal MSI build
- `src/wix/scripts/rebuild.sh` — remove previous output and rebuild MSI from scratch
- `src/wix/scripts/stage-only.sh` — prepare staged payload only for inspection/debugging
- `src/wix/scripts/inspect-payload.sh` — stage payload and print the staged tree for inspection/debugging

It follows the WiX v6 CLI flow:

1. stage payload into `src/wix/out/payload`
2. run `wix extension add` as needed for required extensions
3. run `wix build` with `src/wix/code/Product.wxs` plus bind-time variables such as `PayloadRoot`

This repo intentionally targets the modern WiX CLI instead of the deprecated
WiX v3 `heat.exe` / `candle.exe` / `light.exe` toolchain.

The current package authoring uses built-in file harvesting via WiX v4+/v6
authoring (`<Files ... />`) rooted at the staged `PayloadRoot`.

Packaged vs developer script behavior
------------------------------------

The packaged MSI payload must not ship the developer checkout `scripts/`
directory literally.

Instead, `src/wix/scripts/build.sh` stages packaged-specific launcher
templates from `src/wix/payload/scripts/` into the MSI `scripts/` payload
with this contract:

* packaged `kano-git` / `kog` remain runtime launchers for the installed
  native binary
* packaged `self build` and `self rebuild` are explicitly unsupported
* packaged `self sync` and `self maybe-auto-update` are explicitly unsupported
* packaged `self update` is guidance-only and must not mutate the MSI-owned
  install tree
* packaged `self update-check` may query release metadata through native
  packaged-aware commands, but should first report installed version,
  install root, distribution channel, and package-manager context
* packaged payload includes metadata that marks the install as packaged,
  including current version and distribution channel

Developer-checkout behavior (git sync, self build, self rebuild, git-based
manual self update) remains owned by the repo checkout scripts and is not a
contract of the MSI payload.

CI/release environments that build the MSI must provide WiX v6 CLI support,
either from an installed `wix.exe` or from the .NET tool bootstrap path.
