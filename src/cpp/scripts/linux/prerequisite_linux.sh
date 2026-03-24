#!/usr/bin/env bash
set -euo pipefail

has_pixi_env() {
  [[ -n "${PIXI_PROJECT_ROOT:-}" || -n "${CONDA_PREFIX:-}" ]]
}

have_command() {
  command -v "$1" >/dev/null 2>&1
}

if ! command -v apt-get >/dev/null 2>&1; then
  echo "Unsupported Linux package manager for this script. Please install cmake, ninja-build, gcc-15/g++-15, clang, git manually." >&2
  exit 1
fi

SUDO=""
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "This script needs root privileges (run as root or install sudo)." >&2
    exit 1
  fi
fi

packages=(
  ca-certificates
  gcc-15
  g++-15
  clang
  pkg-config
)

if has_pixi_env && have_command cmake; then
  echo "[prereq][linux] pixi provides cmake; skipping apt package cmake"
else
  packages+=(cmake)
fi

if has_pixi_env && have_command ninja; then
  echo "[prereq][linux] pixi provides ninja; skipping apt package ninja-build"
else
  packages+=(ninja-build)
fi

if has_pixi_env && (have_command python || have_command python3); then
  echo "[prereq][linux] pixi provides python; skipping apt package python3"
else
  packages+=(python3)
fi

if has_pixi_env && have_command git; then
  echo "[prereq][linux] pixi provides git; skipping apt package git"
else
  packages+=(git)
fi

${SUDO} apt-get update
${SUDO} DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"

echo "Linux prerequisites setup complete."
