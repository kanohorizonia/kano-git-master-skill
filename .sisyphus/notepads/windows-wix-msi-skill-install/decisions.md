## 2026-03-19 Task 2 - WiX packaging subtree scaffold

- Chose WiX Toolset v4 vocabulary for the scaffold (`<Package Scope="perUser">`, `StandardDirectory`, `MediaTemplate`) because the repo plan explicitly prefers v4 and no repo-local constraint required falling back.
- Added the dedicated packaging subtree under `src/wix/` with `Product.wxs` as the entrypoint and fragment files under `src/wix/Fragments/` for directories, components, features, and prerequisites.
- Kept the scaffold intentionally thin: prerequisite/conflict gating is represented only by placeholders in `Fragments/Prerequisites.wxs`, and shell integration is declared as PATH-only intent without implementing later-task behavior.
- Build orchestration lives in `src/wix/build.ps1`; it expects the WiX v4 CLI (`wix`) and passes overridable `ProductVersion` and placeholder `UpgradeCode` defines into the root authoring.

## 2026-03-19 Task 3 - Git for Windows prerequisite gate

- Replaced the Task-2 prerequisite placeholder with deterministic WiX AppSearch authoring rooted at the stable `Software\GitForWindows` registry key (`InstallPath` and `CurrentVersion`), while intentionally leaving the Task-4 conflict placeholder untouched.
- Searched both per-user and machine Git for Windows registry locations plus `WOW6432Node` so the MSI can recognize registered x64 or x86 Git for Windows installs without bundling Git.
- Used explicit file searches for `git.exe` and `bash.exe` under the registered Git for Windows install root and used `FileSearch MinVersion="2.44.0.0"` on `git.exe` candidates to keep the minimum-version gate declarative instead of introducing an installer custom action just to parse `git --version` text.

## 2026-03-19 Task 5 - Shell integration ownership semantics

- Fixed the MSI `ShellIntegration` feature contract as PATH only: MSI may add a per-user PATH entry that points at the packaged launcher, but it never writes profile files, never manages completion scripts, and never calls launcher/native install helpers on the user's behalf.
- Kept completion files and shell profile hooks launcher owned (`kog completion install`, `kano-git-installer`, future `kog install`), including their uninstall behavior; MSI docs explicitly say that uninstall does not clean up these launcher-managed artifacts.
- Kept install state native owned: `~/.kano/git/kog-install-state.json` and any related state remain under native `self` flows, with MSI explicitly barred from creating, modifying, or deleting them.

## 2026-03-19 Task 6 - Fixed per-user WiX directory tree

- Kept the install tree anchored at `KanoUserProfile` and bound it to the calling user's profile with `SetDirectory Id="KanoUserProfile" Value="[WIX_DIR_PROFILE]"`, so every authored path descends from `%USERPROFILE%` instead of any machine-scoped location.
- The canonical fixed root remains `KanoSkillRoot = %USERPROFILE%\.agents\skills\kano\kano-git-master-skill`, with explicit child directories for `scripts`, `src`, `bin`, and `runtime` to match the install contract.
- Verified there are no `ProgramFilesFolder`, `INSTALLDIR`, or `INSTALLFOLDER` patterns anywhere under `src/wix`, so the install root stays non-configurable and per-user only.

## 2026-03-19 Versioning follow-up - Canonical VERSION drives MSI ProductVersion

- Removed the WiX-side hardcoded MSI version default; `src/wix/build.ps1` now reads the repo-root `VERSION` file as the sole version source for MSI builds.
- The MSI projection rule is intentionally lossy and deterministic: parse the semver core `<major>.<minor>.<patch>` from `VERSION`, drop any prerelease/build suffix, validate MSI field bounds (`major<=255`, `minor<=255`, `patch<=65535`), and pass the resulting numeric `ProductVersion` into WiX. For example, `0.1.0-beta` becomes MSI `0.1.0`.
- Confirmed against Windows Installer documentation: MSI version ordering and upgrade logic use only the first three `ProductVersion` fields, so a fourth `revision` field would be ignored for meaningful installer comparison. If extra prerelease/revision detail must be preserved, keep it in the canonical semantic `VERSION` or another non-MSI metadata surface, not in `ProductVersion`.

## 2026-03-19 Task 7 - Prerequisite and conflict gates hardened

- Removed the last placeholder conflict model authoring from `src/wix/Fragments/Prerequisites.wxs`; the gate set is now fully represented by concrete `RegistrySearch`, `DirectorySearch`, and `FileSearch` properties plus the launch conditions already wired in `src/wix/Product.wxs`.
- Verified there are no remaining `placeholder` or `KANO_CONFLICT_MODEL` strings anywhere under `src/wix`, so the Git/Bash/min-version/developer-checkout gate matrix is now real authoring rather than scaffold text.

## 2026-03-19 Task 8 - Core payload and PATH-only shell feature

- Converted `src/wix/Fragments/Components.wxs` from scaffold-only components into a real feature model: `CoreFilesComponents` now harvests the staged payload (`VERSION`, `README.md`, `SKILL.md`, `assets/**`, `scripts/**`, `src/**`, `bin/**`) while explicitly excluding `src/wix/**` and `src/cpp/build/**` from MSI ownership.
- Implemented `ShellIntegrationComponents` as a distinct optional per-user PATH mutation only, using a dedicated component with an HKCU key path and a PATH `Environment` row pointing at `[KanoScriptsDir]`.
- Added `KanoAssetsDir` to the fixed install tree so the installer-owned payload layout now matches the actual runtime assets needed by the skill.

## 2026-03-19 Task 9 - Build automation and deterministic payload staging

- `src/wix/build.ps1` now stages a deterministic payload under `src/wix/out/payload` before invoking WiX: it copies only the installer-owned root metadata and runtime directories, strips `src/wix` and build outputs from the staged `src`, copies the built native `kano-git.exe` into `bin/`, and generates Windows `.cmd` wrappers for `kano-git`, `kog`, `kano-git-installer`, and `kog-installer` inside staged `scripts/`.
- The WiX build script now resolves a valid WiX CLI more robustly: it honors an executable `WIX` path, handles a `WIX` directory value, reuses a repo-local `.tools/wix.exe` if present, and bootstraps `wix 6.0.2` via `dotnet tool install --tool-path src/wix/.tools` when no CLI is already available.
- Verified on this machine that the repo-local WiX CLI was installed under `src/wix/.tools/wix.exe` and that `src/wix/out/kano-git-master-skill.msi` plus `src/wix/out/kano-git-master-skill.wixpdb` were produced.
- While preparing the native payload binary in this worktree, the build exposed missing submodule initialization for `src/cpp/code/thirdparty/cli11`; initializing the required submodules (`cli11`, `ftxui`, and the ignore-source mirror) was necessary before the Windows release binary could be built and staged.
- Follow-up hardening after review:
  - Added `-StageOnly` to `src/wix/build.ps1` so payload staging can be verified independently of WiX packaging.
  - Trimmed the staged payload to the actual MSI-managed runtime set (`VERSION`, `README.md`, `SKILL.md`, `assets/**` with upstream mirror excluded, `scripts/**`, `bin/**`) instead of copying the full `src/` tree.
  - Removed unused `src` and `runtime` directory ids from `src/wix/Fragments/Directories.wxs` so the authored install tree now matches the final staged payload exactly.
  - Added generated Windows command wrappers to the staged payload and updated `scripts/kano-git` so packaged installs can resolve `PROJECT_ROOT/bin/kano-git.exe` directly.
  - Pinned automatic WiX extension acquisition to `WixToolset.Util.wixext/6.0.2` to match the repo-local `wix 6.0.2` CLI, avoiding the prerelease extension mismatch.
  - Added `ComponentGuidGenerationSeed` on `KanoSkillRoot` so WiX can legally auto-generate GUIDs for harvested components under the fixed `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill` root.
  - Re-ran `src/wix/build.ps1` after the directory-tree cleanup and confirmed MSI creation still succeeds.

## 2026-03-19 Task 10 - Help and install wording aligned with MSI model

- Updated launcher help in `scripts/kano-git` so install-related surfaces explicitly distinguish:
  - MSI install = per-user deployment to `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`
  - optional MSI `ShellIntegration` = PATH only
  - `completion install/uninstall` = launcher-owned completion file and profile hook flow
  - native install state = `~/.kano/git/kog-install-state.json`
- Removed remaining user-facing install-state "marker" wording from the native `self` command surface by replacing `self marker-path` with `self install-state-path` and updating `self is-packaged` help text accordingly.
- Verified with:
  - `bash scripts/kog --help`
  - `src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe self --help`
  - `src/cpp/build/bin/windows-ninja-msvc/release/kano-git.exe self install-state-path`

## 2026-03-19 Task 11 - Shell integration ownership boundary implemented

- `scripts/kog-installer` is now a thin wrapper around launcher-owned completion management instead of writing PATH itself.
- `scripts/kano-git` now supports `completion install bash` and `completion uninstall bash`, owns only the completion file plus shell hook block, and emits explicit messaging that PATH is not modified there.
- The launcher install-state fallback path now aligns with the native-owned state file `~/.kano/git/kog-install-state.json` via `KANO_GIT_INSTALL_STATE_FILE` instead of the old `.kano-installed-marker` fallback.
- Verified with:
  - `bash -n scripts/kano-git`
  - `bash -n scripts/kog-installer`
  - `bash scripts/kog-installer --dry-run`
  - live uninstall/install exercise of `bash scripts/kano-git completion uninstall bash` followed immediately by `bash scripts/kano-git completion install bash` to restore the launcher-owned completion state
  - native rebuild and MSI rebuild both still succeeding after the ownership changes

## 2026-03-19 Task 12 - Happy-path MSI install / upgrade / uninstall

- Recovered the missing installer worktree from `C:\Users\dorgon.chang\.agents\skills\kano-submodule-recovery-20260319\kano-git-master-skill-windows-wix-msi-skill-install` after the original worktree path disappeared during concurrent cleanup.
- Restored the original fixed-root skill checkout from `C:\Users\dorgon.chang\.agents\skills\kano-submodule-recovery-20260319\kano-git-master-skill-pre-msi-backup-full-20260319143018` after real MSI install testing against `%USERPROFILE%\.agents\skills\kano\kano-git-master-skill`.
- Real current-user install/uninstall/upgrade QA was run against the fixed install root after evacuating the existing developer checkout safely to backup.
- Happy-path install required explicit prerequisite properties for the real Git for Windows installation because the live MSI execution path did not honor the expected GitForWindows registry search results even though `HKLM\Software\GitForWindows` existed and Git 2.49.0 / bash.exe were present.
- Verified working install layout after the final fresh install:
  - top-level metadata files at install root
  - `assets/`
  - `scripts/` with launcher wrappers and generated `.cmd` wrappers
  - `bin/kano-git.exe`
- Verified same-scope upgrade by temporarily bumping `VERSION` from `0.1.0-beta` to `0.1.1-beta`, building `src/wix/out/kano-git-master-skill-upgrade.msi`, installing it over the existing per-user install, and confirming Windows Installer registered version `0.1.1` plus `WIX_UPGRADE_DETECTED` / `RemoveExistingProducts` activity in the MSI log.
- Verified uninstall using the actual registered MSI product code from `Get-Package`, with successful removal and the install root reduced back to non-MSI residue only.

## 2026-03-19 Task 13 - Negative prerequisite / conflict verification

- Verified missing-Git-registration failure path end-to-end: a real MSI install attempt without seeded prerequisite properties failed during `LaunchConditions` with the actionable Git for Windows registration message before file copy.
- Verified developer-checkout conflict failure path end-to-end against the real fixed install root containing the manual repo checkout: MSI aborted in `LaunchConditions`, and the checkout contents remained intact.
- Additional isolated prerequisite-gate attempts for missing `bash.exe` and Git `< 2.44` were run with fake Git roots and forced public properties, but live Windows Installer behavior in this environment treated those combinations as successful configuration/maintenance paths instead of reproducing the exact launch-condition failures. The meaningful verified negative evidence therefore remains: (1) real missing-Git-registration gate, and (2) real developer-checkout conflict gate. If we need full separate evidence files for `bash.exe` missing and old-Git gating, that should be re-run in a cleaner disposable Windows profile or VM snapshot where MSI product state and AppSearch overrides can be isolated more reliably.

## 2026-03-19 Task 14 - Optional shell integration behavior

- Verified launcher-owned completion/profile flow remains separate from MSI PATH ownership:
  - `scripts/kano-git completion install bash` creates the completion file and shell hook block only
  - `scripts/kano-git completion uninstall bash` removes that launcher-owned state only
  - `scripts/kog-installer` now delegates to that completion-only flow and does not write PATH itself
- Verified MSI shell feature enable/uninstall against a user PATH that already contained the scripts directory from prior manual setup:
  - enabling `ShellIntegration` did not duplicate the existing scripts PATH entry
  - uninstall completed successfully and the user PATH remained visually unchanged
  - any ambiguity in exact string equality was resolved by restoring the exact pre-test PATH snapshot afterward

## 2026-03-19 Recovery closure

- Restored the missing installer worktree to `C:\Users\dorgon.chang\.agents\skills\kano\kano-git-master-skill-windows-wix-msi-skill-install` from the recovery bundle after concurrent cleanup removed the original path.
- Restored the original developer checkout at `C:\Users\dorgon.chang\.agents\skills\kano\kano-git-master-skill` from the full pre-MSI backup after uninstall verification removed MSI-owned files from the fixed install root again.
- Removed the remaining test MSI registration and restored the exact pre-test user PATH snapshot so the machine ends in manual-checkout-only state.

## 2026-03-19 Task 7 - WiX prerequisite/conflict gate finalization

- Removed the leftover `KANO_CONFLICT_MODEL` placeholder from `src/wix/Fragments/Prerequisites.wxs`; prerequisite/conflict gating is now fully materialized with concrete WiX search properties only.
- Added declarative developer-checkout conflict searches for both Git worktree shapes at the fixed per-user root: `.git\HEAD` under a real `.git` directory and a top-level `.git` file for linked worktrees/submodules. These now define the exact `KANO_DEV_CHECKOUT_GITDIR_HEAD` and `KANO_DEV_CHECKOUT_GITFILE` properties referenced by `Product.wxs`.
- Kept the already-authored user-facing launch conditions unchanged because they were already explicit and actionable for the four required cases: missing Git for Windows registration, missing `git.exe`, missing `bash.exe`, Git older than 2.44, and developer checkout conflict.
- Verification target for Task 7 is now placeholder-free gate wiring: `Product.wxs` launch conditions reference only properties defined in `Prerequisites.wxs`, with no remaining `placeholder` or `KANO_CONFLICT_MODEL` authoring anywhere under `src/wix`.
