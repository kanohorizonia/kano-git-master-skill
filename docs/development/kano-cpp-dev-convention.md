# Kano C++ Development Convention

This document captures repo-local C++ conventions that should stay consistent across `src/cpp/`.

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
