# Kano C++ Development Convention

This document captures repo-local C++ conventions that should stay consistent across `src/cpp/`.

## Third-party dependency sourcing policy

- Keep ordinary upstream dependencies out of `src/cpp/code/thirdparty/`.
- Default to CMake `FetchContent` so upstream libraries are resolved into the build's `_deps` area without carrying them as git submodules.
- Reserve `src/cpp/code/thirdparty/` git submodules for dependencies we intentionally customize, patch locally, or maintain as a fork.

Why:

- it keeps the repo smaller and reduces routine submodule maintenance
- it makes the boundary clear between unmodified upstream code and Kano-owned vendor forks
- it aligns `cli11` / `ftxui` with the existing `Catch2` / `tomlplusplus` fetch-first model

## Current dependency rule of thumb

- unmodified upstream dependency → `FetchContent` / `_deps`
- Kano-customized or fork-maintained dependency → `src/cpp/code/thirdparty/` git submodule

## Public header include convention

- When a target already exports its `public/` directory through CMake include paths, implementation files should include public headers by logical include name only.
- Prefer:

```cpp
#include "version.hpp"
```

- Avoid relative-path includes into sibling `public/` directories such as:

```cpp
#include "../public/version.hpp"
```

Why:

- it matches the target's declared include surface
- it keeps private sources independent from local directory reshuffles
- it makes exported/public API boundaries clearer during reviews

## Rule of thumb

- `public/` headers: include by exported header name
- `private/` headers: include by local/private path only when they are intentionally internal to that target
