# SkyrimCameraDisocclusion_Improve

This repository contains a Skyrim SE SKSE plugin focused on third-person camera disocclusion (see-through behavior when the camera is blocked).

The project now supports a CMake + VS2022 workflow that is reproducible in VS Code while preserving the original source intent.

## Requirements

- Visual Studio 2022 with Desktop development with C++
- CMake 3.21+
- `VCPKG_ROOT` pointing to your local vcpkg checkout
- Local registries used by this workspace:
  - `C:/SKSE_Development/registries/vcpkg`
  - `C:/SKSE_Development/registries/vcpkg-colorglass`

## Build

Debug:

```powershell
cmake --preset debug
cmake --build --preset build-debug
```

Release:

```powershell
cmake --preset release
cmake --build --preset build-release
```

## Current Build Route

- Default route builds against local `../CommonLibSSE` when available.
- Fallback route uses package-managed `CommonLibSSE` (`find_package`).
- This is controlled by `USE_LOCAL_COMMONLIBSSE` in `CMakeLists.txt`.

## Output

- Debug DLL: `build/vs-debug/Debug/skyrimcameradisocclusion.dll`
- Release DLL: `build/vs-release/Release/skyrimcameradisocclusion.dll`

If `SKYRIM_MODS_FOLDER` or `SKYRIM_FOLDER` is set, post-build copy places the plugin into `SKSE/Plugins` automatically.

## Notes

- SKSE menu integration is enabled in this branch (when SKSE Menu Framework runtime is present).
- Renderer clipping logic is restored on the current CommonLibSSE route through compatibility accessors in `src/Hook.cpp`.
- `xmake.lua` remains in the repository, but CMake presets are the validated workflow for this branch.

## Upstream Review Guide

For submitting these additions back to upstream authors, see:

- `UPSTREAM_PATCH_REVIEW.md`
