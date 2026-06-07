# SkyrimCameraDisocclusion_Improve Operations Quickstart

Use case: daily builds, fast deployment, and a short pre-launch check before testing in game.

## 1. One-Time Environment Setup

- Install Visual Studio 2022 with Desktop development with C++
- Install CMake (3.21+)
- Set environment variable `VCPKG_ROOT` to your local vcpkg directory
- Ensure these local registry paths exist:
  - `C:/SKSE_Development/registries/vcpkg`
  - `C:/SKSE_Development/registries/vcpkg-colorglass`

Optional but recommended:

- Keep local `CommonLibSSE` checkout at `../CommonLibSSE` relative to this repo

## 2. Daily VS Code Workflow

Recommended daily flow:

1. Configure once per config (`debug` or `release`)
2. Build repeatedly with matching build preset

## 3. Terminal Build Commands

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

## 4. Output Paths

- Release DLL: `build/vs-release/Release/skyrimcameradisocclusion.dll`
- Debug DLL: `build/vs-debug/Debug/skyrimcameradisocclusion.dll`

Current validated quick check is Debug build output first.

## 5. Deploy to MO2 (Current Test Path)

Example target directory:

`C:/Users/ysaee/AppData/Local/ModOrganizer/Skyrim Special Edition/mods/Skyrim Camera Disocclusion System/SKSE/Plugins`

Minimum files to copy:

- `skyrimcameradisocclusion.dll`
- `skyrimcameradisocclusion.ini`

Optional:

- `skyrimcameradisocclusion.pdb` (useful for crash diagnostics)

PowerShell quick deploy example:

```powershell
$repo = "C:/SkyrimSETools/Skyrim SKSE or Scripts/SKSE Workspace/SkyrimCameraDisocclusion_Improve"
$dst  = "C:/Users/ysaee/AppData/Local/ModOrganizer/Skyrim Special Edition/mods/Skyrim Camera Disocclusion System/SKSE/Plugins"

New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item "$repo/build/vs-release/Release/skyrimcameradisocclusion.dll" "$dst/skyrimcameradisocclusion.dll" -Force

if (Test-Path "$repo/dist/SKSE/Plugins/skyrimcameradisocclusion.ini") {
    Copy-Item "$repo/dist/SKSE/Plugins/skyrimcameradisocclusion.ini" "$dst/skyrimcameradisocclusion.ini" -Force
}

if (Test-Path "$repo/build/vs-release/Release/skyrimcameradisocclusion.pdb") {
    Copy-Item "$repo/build/vs-release/Release/skyrimcameradisocclusion.pdb" "$dst/skyrimcameradisocclusion.pdb" -Force
}
```

## 6. 30-Second Pre-Launch Check

- Confirm DLL timestamp in target directory is the newest build
- Confirm INI file exists and contains expected values
- Confirm MO2 priority/order is not overriding this plugin with another same-name DLL

If using Debug for test:

- Copy from `build/vs-debug/Debug/` instead of `build/vs-release/Release/`

## 7. Common Issues

1) Configure stage fails with missing CommonLibSSE
- Check `VCPKG_ROOT`
- Check that registry paths in `vcpkg-configuration.json` exist on this machine
- If local route is expected, confirm `../CommonLibSSE` exists

2) Terminal build fails with missing `cl`
- This is a common shell environment issue
- Use the provided VS2022 CMake presets in this repository

3) Build succeeds but in-game behavior differs from expectation
- Review compatibility sections in `src/Hook.cpp` (camera/cell/shadow view data access)
- Verify correct DLL is loaded in MO2

4) No in-game effect at all
- Confirm the correct MO2 mod is enabled
- Confirm target DLL is the latest build
- Confirm no same-name plugin is overriding it

## 8. Reference

- Build adaptation summary: `BUILD_ADAPTATION_NOTES.md`
- Main project overview: `README.md`
