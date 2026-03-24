#!/usr/bin/env bash
set -euo pipefail

has_pixi_env() {
  [[ -n "${PIXI_PROJECT_ROOT:-}" || -n "${CONDA_PREFIX:-}" ]]
}

have_command() {
  command -v "$1" >/dev/null 2>&1
}

if ! command -v xcode-select >/dev/null 2>&1; then
  echo "xcode-select not found. Install Xcode Command Line Tools first." >&2
  exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Installing Xcode Command Line Tools..."
  xcode-select --install || true
  echo "Please complete the Command Line Tools installation, then re-run this script." >&2
  exit 1
fi

packages=(llvm)

if has_pixi_env && have_command cmake; then
  echo "[prereq][macos] pixi provides cmake; skipping brew package cmake"
else
  packages+=(cmake)
fi

if has_pixi_env && have_command ninja; then
  echo "[prereq][macos] pixi provides ninja; skipping brew package ninja"
else
  packages+=(ninja)
fi

if has_pixi_env && have_command git; then
  echo "[prereq][macos] pixi provides git; skipping brew package git"
else
  packages+=(git)
fi

if has_pixi_env && (have_command python || have_command python3); then
  echo "[prereq][macos] pixi provides python; skipping brew package python"
else
  packages+=(python)
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required. Install from https://brew.sh and re-run." >&2
  exit 1
fi

if ! brew update; then
  echo "Warning: brew update failed; continuing with no-auto-update install attempt." >&2
fi

HOMEBREW_NO_AUTO_UPDATE=1 brew upgrade "${packages[@]}" || true
HOMEBREW_NO_AUTO_UPDATE=1 brew install "${packages[@]}"

echo "macOS prerequisites setup complete."
