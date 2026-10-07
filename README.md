# CorvoBody - first-person body for Dishonored (+ Body Swap)

Source code for **CorvoBody** on Nexus Mods: https://www.nexusmods.com/dishonored/mods/453

See your own body in first person in Dishonored (2012), like Dishonored 2. The **Body Swap** add-on lets you play
as Corvo, Daud or Daud's High Overseer disguise (body, arms, blade, powers and crossbow), switchable from the
pause menu through [Dishonored Mod Loader](https://www.nexusmods.com/dishonored/mods/383).

The mod contains no game assets: it uses the models and animations already installed with the game's DLC.

## What is in here

| Path | What it is |
|---|---|
| `src/main.cpp` | CorvoBody: a `dinput8.dll` proxy that hooks the game with MinHook, builds the visible body from the DLC models, animates it and keeps it out of the camera |
| `src/baked_settings.h` | the tuned settings (the mod has no ini or hotkeys) |
| `src/dinput8.def` | exports: `DirectInput8Create` (forwarded to the system `dinput8.dll` or a chain-loaded mod), `BlinkBootstrap_Register` (forwarded for mods that expect it from `dinput8.dll`) and the Body Swap API |
| `bodyswap/` | `BodySwap.dll`, a Dishonored Mod Loader plugin that adds the Body Swap menu and passes the choices to CorvoBody |
| `patches/codered-sdk.patch` | three small fixes applied to the CodeRed SDK after it is fetched |
| `shim/xlocale` | a one-line header shim so the SDK compiles with clang |
| `build.ps1` | fetches the third-party code and builds both DLLs |

## Building

Requirements: Windows, [Git](https://git-scm.com/), and [Zig 0.17.0](https://ziglang.org/download/)
(used as the C/C++ compiler; `winget install zig.zig`).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

If `zig` isn't on your PATH, pass it: `-Zig C:\path\to\zig.exe`. The first run fetches the third-party code (below)
into `external\` and compiles the SDK (about a minute). Output:

- `build\dinput8.dll` - CorvoBody (goes next to `Dishonored.exe`, in `Binaries\Win32`)
- `build\BodySwap.dll` - Body Swap (goes in `Binaries\Win32\Mods\plugins`)

`-Install` copies both into the Steam install (`-Game <Binaries\Win32 folder>` to change it).

The release DLLs for version 2.0 were built from this source with these steps. A clean build is identical to them
apart from the link timestamp and the build ID, which change on every build.

## Supported game version

The Steam release of `Dishonored.exe` (32-bit), SHA-256
`66443F3D68A6C658B0ED943260C3EB2F4CC9E3400D5809A94D6EC41EB725E17E`. At startup the mod checks the game code it
hooks and switches itself off if it doesn't match.

## Third-party code

Not stored in this repository; `build.ps1` fetches it at fixed commits:

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, commit `8af6b4a` - BSD 2-Clause licence.
- The CodeRed SDK for Dishonored (generated with [CodeRed Generator](https://github.com/CodeRedModding/CodeRed-Generator)),
  from [ectrc/dismod](https://github.com/ectrc/dismod) `external/sdk`, commit `2d308cd`, then
  `patches/codered-sdk.patch`: a clang compile fix, a safer `FName::ToString`/`IsValid`, and a null check in
  `UObject::GetFullName`.

## Licence

CorvoBody and Body Swap are released under the MIT licence - see [LICENSE](LICENSE). The third-party code above
keeps its own licence. Dishonored is (c) Arkane Studios / Bethesda Softworks.
