# CorvoBody - first-person body for Dishonored (+ Body Swap)

Source code for **CorvoBody** on Nexus Mods: https://www.nexusmods.com/dishonored/mods/453

See your own body in first person in Dishonored (2012), like Dishonored 2 - and, since 3.0, its real-time shadow
from the sun and from lamps, following every pose, weapon and item in your hands. The **Body Swap** add-on lets you
play as Corvo, Daud or Daud's High Overseer disguise (body, arms, blade, powers and crossbow), switchable from the
pause menu through [Dishonored Mod Loader](https://www.nexusmods.com/dishonored/mods/383). Since 3.1 it can also
choose who Summon Assassin brings: the regular assassin, masked Corvo, Daud or an Overseer hound.

**4.0** adds a third-person mode (V on the keyboard, or L3 + R3 together on a pad) with a free orbit camera while your
hands are empty, and Daud's **Pull** (The Brigmore Witches) in Windblast's slot - for objects, bodies and, upgraded,
living enemies, which it lifts, holds and yanks to you as in the DLC (Body Swap row "Windblast": Windblast / Pull).

This repository contains code only. The mod uses the models and animations already installed with the game's DLC.
The Nexus download (3.0 and later) also includes, none of which is in this repository: high-detail upscales of the player
body textures (`CorvoBody\hd\*.dds`), the game's main menu package with a "CorvoBody Version 3" subtitle added
under the logo (`Dishonored_MainMenu.upk`), and Dishonored Mod Loader by Crimsab.

## What is in here

| Path | What it is |
|---|---|
| `src/main.cpp` | CorvoBody: a `dinput8.dll` proxy that hooks the game with MinHook, builds the visible body from the DLC models, animates it, keeps it out of the camera and casts the player's shadow. It also swaps the player body's textures for the high-detail ones through a Direct3D 9 `SetTexture` hook, and keeps the `Dishonored_MainMenu.upk` size entry in `DishonoredGame\PCConsoleTOC*.txt` in step with the installed title-screen package |
| `src/baked_settings.h` | the tuned settings (the mod has no ini; its one hotkey is the third-person toggle) |
| `src/dinput8.def` | exports: `DirectInput8Create` (forwarded to the system `dinput8.dll` or a chain-loaded mod), `BlinkBootstrap_Register` (forwarded for mods that expect it from `dinput8.dll`) and the Body Swap API |
| `src/dinput8.rc` | version information for `dinput8.dll` (4.0.1) |
| `bodyswap/` | `BodySwap.dll`, a Dishonored Mod Loader plugin that adds the Body Swap menu and passes the choices to CorvoBody; `BodySwap.rc` is its version information (4.0) |
| `tools/harden_pe.py` | finishing step for the zig-linked `dinput8.dll`: drops the debug sections and symbol table and sets the PE checksum, without changing a code or data byte (3.2) |
| `patches/codered-sdk.patch` | three small fixes applied to the CodeRed SDK after it is fetched |
| `shim/xlocale` | a one-line header shim so the SDK compiles with clang |
| `build.ps1` | fetches the third-party code and builds both DLLs |

## Building

Requirements: Windows, [Git](https://git-scm.com/), [Zig 0.17.0](https://ziglang.org/download/) (the compiler for
`dinput8.dll`; `winget install zig.zig`), Visual Studio 2022 or its free Build Tools with "Desktop development with
C++" (the compiler for `BodySwap.dll`, since 3.2), and Python 3 with `pefile` (`pip install pefile`, for
`tools/harden_pe.py`).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

If `zig` isn't on your PATH, pass it: `-Zig C:\path\to\zig.exe`. The first run fetches the third-party code (below)
into `external\` and compiles the SDK (about a minute). Output:

- `build\dinput8.dll` - CorvoBody (goes next to `Dishonored.exe`, in `Binaries\Win32`)
- `build\BodySwap.dll` - Body Swap (goes in `Binaries\Win32\Mods\plugins`)

`-Install` copies both into the Steam install (`-Game <Binaries\Win32 folder>` to change it).

The release DLLs were built with these steps from the matching tag (`v3.0`, `v3.1`, `v3.2`, `v4.0`, `v4.0.1`).

Version 4.0.1 (tag `v4.0.1`) fixes the sword staying invisible after leaving third person while it was being
holstered or swapped, and shows Blink in third person as someone watching would see it. The camera holds a moment
where you stood, then glides after you. Daud's assassins' black flakes burst where you vanish and where you land
(the game's own NPC teleport effects, spawned on the body's bones). Blink's first-person lens burst is kept out of
the third-person view. Only `src/main.cpp` and `src/dinput8.rc` changed; `BodySwap.dll` is the 4.0 file (same hash).

- `dinput8.dll` SHA-256 `313701D88E6DC200E5BBA5BF05D8F803FF1331E89D98BB3BDA68F220D47959CB` - a clean build has the
  same bytes in every section; only the link timestamp in the header (and so the checksum) changes
- `BodySwap.dll` SHA-256 `4BAA264644DD2238869BC206CDB40719685634B017AC5CC70DDBA3A9A57D93F8` (as in 4.0)

Version 4.0 (tag `v4.0`) adds the third-person mode and Daud's Pull, and includes the 3.3 fix below (3.3 was released on
Nexus but not tagged here; its changes are part of the 4.0 commit). Pull on campaign NPCs works by giving the game's
campaign NPCs and Corvo's pawn, when the engine allocates them, the extra room the DLC's own NPC and Daud pawn classes
have, so the DLC's Pull code runs on them unchanged - see "Pull on people" in `src/main.cpp`. The idea of Pull for
Corvo came from ClayHasLlamas' "Daud's Pull for Corvo" mod on Nexus; CorvoBody's version is written separately and
shares no code with it.

- `dinput8.dll` SHA-256 `42C62DFFD7515DAA32241886D5F6468A6A3CAE962F7A01C6E5734800E997D55D` - a clean build has the same bytes in every
  section; only the link timestamp in the header (and so the checksum) changes
- `BodySwap.dll` SHA-256 `4BAA264644DD2238869BC206CDB40719685634B017AC5CC70DDBA3A9A57D93F8` (MSVC, `/Brepro`: byte-identical when rebuilt with
  the same compiler)

Version 3.3 fixes a crash loading a save that holds Daud's wristbow bolts while every Body Swap choice is on Corvo: the
preload of Daud's package before a level is restored (added in 3.1) no longer depends on the Body Swap choices, and is
skipped (with a log line) when the engine can't find the package (DLC not installed). Only `src/main.cpp` and
`src/dinput8.rc` changed; `bodyswap/` is unchanged, so `BodySwap.dll` is the 3.2 file (same hash).

- `dinput8.dll` SHA-256 `247DF9BE7904B1183013B7642383516708D7EF5B4E00E43EE00B62873989A9F0` - a clean build has the same
  bytes in every section; only the link timestamp in the header (and so the checksum) changes from build to build
- `BodySwap.dll` SHA-256 `4030BAB227D537C60F655A199AB7192E26E09042282BFFAEE252DF3B3C0DD821` (as in 3.2)

Version 3.2 is the 3.1 code (`src/` and `bodyswap/` unchanged apart from the two version-info files), rebuilt so that
antivirus engines no longer flag it: 3.0's `BodySwap.dll` was flagged by 23 of 71 engines on VirusTotal (generic and
machine-learning detections such as "Gen:Variant.Babar") because of how it was compiled - a small zig/mingw DLL with
no version information, an appended symbol table and no checksum. In 3.2 `BodySwap.dll` is built with MSVC and both
DLLs carry version information and a valid checksum. VirusTotal: `BodySwap.dll` 0/71, `dinput8.dll` 0/70.

- `dinput8.dll` SHA-256 `0E9E4014F94200DCE376F5AB868100CD21EADCF4D3748D9B7FD9002BFAC044E4` - a clean build has the same
  bytes in every section; only the link timestamp in the header (and so the checksum) changes from build to build
- `BodySwap.dll` SHA-256 `4030BAB227D537C60F655A199AB7192E26E09042282BFFAEE252DF3B3C0DD821` - byte-identical when
  rebuilt with the same MSVC version (VS 2022 17.14, compiler 19.44; `/Brepro` makes it deterministic)

Version 3.1 (a clean build is identical apart from the link timestamp and the build ID):

- `dinput8.dll` SHA-256 `D51A29F83B6A2345A956F9BAA6C0EAD7DCC07F282729A1F0114F2D7B6E7CE307`
- `BodySwap.dll` SHA-256 `369DF861594EC8789772515D766335390FD96163C3EC72D9DF44D7883C3F8893`

**Building an older version (2.0, 3.0, 3.1):** the `build.ps1` in those commits doesn't apply its SDK patch in a
fresh git clone (`git apply` takes the patch's paths as relative to the repository root and skips them without an
error, and with Git for Windows' default `core.autocrlf=true` the patch's line ends don't match the SDK's), so a
fresh build of them stops with a compile error in `defs.hpp`. Use the fixed script from `main`, which changes only
how the patch is applied (for these versions - they have no `.rc` files - it builds both DLLs with zig exactly as
they were released):

```powershell
git checkout v3.0            # or v3.1, or e006d9f for 2.0
git checkout main -- build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1
```

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
