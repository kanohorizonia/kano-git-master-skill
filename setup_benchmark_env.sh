#!/usr/bin/env bash
set -e

BASE_DIR="/d/_work/_Kano/kano-benchmark-env"
rm -rf "$BASE_DIR"
mkdir -p "$BASE_DIR"

create_repo() {
  local name=$1
  mkdir -p "$name"
  pushd "$name" > /dev/null
  git init
  touch file.txt
  echo "dummy content for $name" > file.txt
  git add file.txt
  git commit -m "Initial commit for $name"
  popd > /dev/null
}

cd "$BASE_DIR"
mkdir -p external
cd external
create_repo sub-reg-1
create_repo sub-reg-2
create_repo sub-reg-3

# Add nested repo into sub-reg-1
cd sub-reg-1
mkdir -p nested-unreg
cd nested-unreg
git init
echo "nested" > nested.txt
git add nested.txt
git commit -m "Nested"
cd ../..

# Go back to root and add submodules
cd "$BASE_DIR"
mkdir -p benchmark-root
cd benchmark-root
git init
# Use relative paths for submodules to keep it portable/clean
git -c protocol.file.allow=always submodule add ../external/sub-reg-1 sub-reg-1
git -c protocol.file.allow=always submodule add ../external/sub-reg-2 sub-reg-2
git -c protocol.file.allow=always submodule add ../external/sub-reg-3 sub-reg-3

# Create unregistered subrepos inside root
create_repo sub-unreg-1
create_repo sub-unreg-2
create_repo sub-unreg-3

# Final commit in root
git add .
git commit -m "Setup benchmark environment"
echo "COMPLETED_SETUP"
