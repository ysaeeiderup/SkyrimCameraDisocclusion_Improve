# SkyrimCameraDisocclusion_Improve Build Adaptation Summary

Updated: 2026-06-07

## Scope of the Final Patch

The final commit intentionally keeps only these tracked changes:

- `CMakeLists.txt` (new)
- `CMakePresets.json` (new)
- `vcpkg-configuration.json` (new)
- `vcpkg.json` (new)
- `src/main.cpp` (modified)
- `src/Hook.h` (modified)
- `src/Hook.cpp` (modified)

SKSE menu integration code is not included in this final patch.

## What Changed

### 1. Build and Dependency Route

- Added a reproducible VS2022+CMake build path.
- Added vcpkg manifest/registry config used by this workspace.
- `CMakeLists.txt` now supports:
  - local `../CommonLibSSE` route (`USE_LOCAL_COMMONLIBSSE=ON`), and
  - package-managed `find_package(CommonLibSSE)` fallback.

### 2. Plugin Entry and Metadata

- `src/main.cpp` now provides manual plugin metadata in AE mode.
- Keeps `kDataLoaded` listener for renderer-dependent hook initialization.

### 3. Hook Compatibility and Renderer Clipping

- `src/Hook.h` adds SE/AE offset helper (`RelocateSEAE`) and extended stripper API declarations.
- `src/Hook.cpp` adds compatibility accessors for current CommonLibSSE route:
  - camera position / loaded cell data access
  - shadow view data acquisition
  - explicit matrix element/multiply helpers for projection updates
- Renderer clipping branch is active again (not a no-op in legacy path).

## Functional Impact

- Goal behavior is preserved: third-person disocclusion logic remains active.
- Implementation differs from upstream's original API usage because current CommonLibSSE route exposes different interfaces.
- In-game validation was performed after rebuild and reported stable behavior.

## Build Verification

Verified commands:

```powershell
cmake --preset debug
cmake --build --preset build-debug
```

Recent successful output target:

- `build/vs-debug/Debug/skyrimcameradisocclusion.dll`

## Notes for Maintainers

- If exact upstream source-level parity is required, consider maintaining a separate branch that follows the original dependency/API path.
- This branch prioritizes reproducible local build + validated runtime behavior on the current toolchain.
