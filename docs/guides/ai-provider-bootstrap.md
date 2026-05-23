# AI Provider Bootstrap

## Copilot CLI bootstrap

This release supports automatic Copilot CLI bootstrap on Windows with WinGet.

Command:

```bash
./scripts/kog ai bootstrap copilot
```

Dry-run:

```bash
./scripts/kog ai bootstrap copilot --dry-run
```

The bootstrap helper script is:

- `src/shell/bootstrap/windows/ensure-copilot-cli.ps1`

It performs:

- WinGet detection order:
  - `winget --version`
  - `%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe --version`
- Copilot path detection and optional install in explicit bootstrap mode.
- Optional shim creation at `%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\copilot.cmd`.
- User PATH updates only (WindowsApps + WinGet Links).
- Process PATH refresh for the current shell.

Verification commands:

```bash
copilot --version
copilot -s --model auto --no-color --stream off --no-ask-user -p "Reply exactly: hello world"
```

## Normal commit/plan AI flow

Normal `kog commit -ai` and plan AI flows do not install Copilot by default.

If Copilot is missing, guidance is printed:

- Windows + WinGet available: suggests `winget install GitHub.Copilot` and `kog ai bootstrap copilot`.
- Windows + WinGet unavailable: suggests repairing App Installer / WinGet.
- Linux/macOS: manual setup guidance only.

## Config keys

```toml
[ai.model]
selection = "auto"

[ai.model.kog_auto]
change_thresholds = [5, 40, 100]
models = [
  "copilot/gpt-5-mini",
  "copilot/claude-haiku-4.5",
  "copilot/gpt-5.4?reasoning=medium",
  "copilot/gpt-5.4?reasoning=high"
]

[ai.provider.copilot.bootstrap]
auto_install = false
```

Notes:

- `ai.model.auto.*` remains deprecated-compatible input.
- Prefer `ai.model.kog_auto.*` for KOG routing policy.
- Automatic installer support for Linux/macOS is out of scope for this release.
