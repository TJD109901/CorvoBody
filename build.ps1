# Builds CorvoBody (dinput8.dll, with Zig) and the Body Swap add-on (BodySwap.dll, with Visual Studio) for Dishonored
# (32-bit).
#
# Usage:   powershell -ExecutionPolicy Bypass -File build.ps1 [-Zig path\to\zig.exe] [-Install] [-Game folder]
# Needs:   Git; Zig 0.17.0 (https://ziglang.org/download/ - or: winget install zig.zig); Visual Studio 2022 or its Build
#          Tools with the C++ x86 tools ("Desktop development with C++"); Python 3 with pefile (pip install pefile).
# Output:  build\dinput8.dll and build\BodySwap.dll
#
# Since 3.2 both DLLs carry version information and a valid PE checksum, and dinput8.dll ships without its debug
# sections (tools\harden_pe.py): 3.0's files were flagged by antivirus ML/generic detections for missing exactly that.
# BodySwap.dll is built with MSVC (static CRT, deterministic /Brepro), which reads as an ordinary Windows plugin.
#
# Third-party code is not stored in this repository; it is fetched at fixed commits into external\:
#   MinHook (BSD 2-Clause)            https://github.com/TsudaKageyu/minhook
#   CodeRed SDK for Dishonored        https://github.com/ectrc/dismod  (external/sdk), then patches\codered-sdk.patch
param(
    [string]$Zig = 'zig',
    [switch]$Install,
    [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\Dishonored\Binaries\Win32',
    [string]$MinHookRepo = 'https://github.com/TsudaKageyu/minhook',
    [string]$DismodRepo = 'https://github.com/ectrc/dismod'
)
$ErrorActionPreference = 'Stop'
$MinHookCommit = '8af6b4acae5a9388fd742b56fa79ece89d96f823'
$DismodCommit  = '2d308cd7ef1cceea1afb0fd95513927cd5c9e903'

$root = $PSScriptRoot
$ext  = Join-Path $root 'external'
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force $ext, $out, "$out\sdk" | Out-Null

function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# ---- Third-party sources (fetched once) ----
function Get-Repo([string]$url, [string]$commit, [string]$dir) {
    if (Test-Path "$dir\.git") { return }
    Write-Host "Fetching $url @ $($commit.Substring(0, 8)) ..."
    Invoke-Checked "clone $url" { git -c core.autocrlf=false clone -q --no-checkout $url $dir }
    Invoke-Checked "checkout $commit" { git -C $dir -c core.autocrlf=false checkout -q $commit }
}
Get-Repo $MinHookRepo $MinHookCommit "$ext\minhook"
if (-not (Test-Path "$ext\sdk")) {
    Get-Repo $DismodRepo $DismodCommit "$ext\dismod"
    Copy-Item "$ext\dismod\external\sdk" "$ext\sdk" -Recurse
    Write-Host 'Applying patches\codered-sdk.patch ...'
    # In a git clone of this repository 'git apply' takes the patch's paths as relative to the repository root and
    # skips them without an error ("Skipped patch"), so repository discovery is stopped above external\ while it runs.
    # The SDK is checked out with LF line ends (core.autocrlf=false above); the patch is applied from an LF copy, since
    # Git for Windows' default core.autocrlf=true checks patches\codered-sdk.patch out with CRLF.
    $lfPatch = Join-Path $ext 'codered-sdk.lf.patch'
    [IO.File]::WriteAllText($lfPatch, ([IO.File]::ReadAllText((Join-Path $root 'patches\codered-sdk.patch')) -replace "`r`n", "`n"))
    $ceiling = $env:GIT_CEILING_DIRECTORIES
    $env:GIT_CEILING_DIRECTORIES = $root
    Push-Location $ext
    try { Invoke-Checked 'git apply' { git apply -p1 $lfPatch } }
    finally { Pop-Location; $env:GIT_CEILING_DIRECTORIES = $ceiling; Remove-Item $lfPatch -ErrorAction SilentlyContinue }
    if (-not (Select-String -Path "$ext\sdk\include\defs.hpp" -Pattern 'patched for clang' -SimpleMatch -Quiet)) {
        Remove-Item "$ext\sdk" -Recurse -Force
        throw 'patches\codered-sdk.patch was not applied to external\sdk'
    }
}

# ---- Compile ----
$common = @('c++', '-target', 'x86-windows-gnu', '-std=c++20', '-O2', '-w', '-fms-extensions',
            '-I', "$root\shim", '-I', "$ext\sdk\include", '-I', "$ext\minhook\include", '-I', "$root\src")
function Invoke-Zig([string[]]$zigArgs) {
    & $Zig @zigArgs
    if ($LASTEXITCODE -ne 0) { throw "zig failed: $($zigArgs[-1])" }
}

# SDK: only the packages the mod uses (slow, built once).
$sdkUnits = 'Core_classes', 'Engine_classes', 'GameFramework_classes', 'DishonoredGame_classes'
if (-not (Test-Path "$out\sdk\DishonoredGame_classes.o")) {
    Invoke-Zig ($common + @('-c', "$ext\sdk\include\defs.cpp", '-o', "$out\sdk\defs.o"))
    foreach ($u in $sdkUnits) {
        Write-Host "SDK $u ..."
        Invoke-Zig ($common + @('-c', "$ext\sdk\src\$u.cpp", '-o', "$out\sdk\$u.o"))
    }
}

# MinHook (C).
$mhObj = foreach ($f in Get-ChildItem "$ext\minhook\src" -Recurse -Filter *.c) {
    $o = "$out\mh_$($f.BaseName).o"
    if (-not (Test-Path $o)) {
        & $Zig cc -target x86-windows-gnu -O2 -w -I "$ext\minhook\include" -c $f.FullName -o $o
        if ($LASTEXITCODE -ne 0) { throw "minhook failed: $($f.Name)" }
    }
    $o
}

# CorvoBody.
$modObj = foreach ($f in Get-ChildItem "$root\src" -Filter *.cpp) {
    $o = "$out\mod_$($f.BaseName).o"
    Invoke-Zig ($common + @('-c', $f.FullName, '-o', $o))
    $o
}
$sdkObj = @("$out\sdk\defs.o") + ($sdkUnits | ForEach-Object { "$out\sdk\$_.o" })
# 3.2 and later have src\dinput8.rc; for an older version (this script checked out over tag v3.1, v3.0 or 2.0) the DLL
# is linked exactly as it was released, without version info and finishing step.
$v32 = Test-Path "$root\src\dinput8.rc"
$resObj = @()
if ($v32) {
    Invoke-Zig @('rc', '/:output-format', 'coff', '/:target', 'x86', "$root\src\dinput8.rc", "$out\dinput8_version.o")
    $resObj = @("$out\dinput8_version.o")
}
Invoke-Zig (@('c++', '-target', 'x86-windows-gnu', '-shared', '-o', "$out\dinput8.dll",
              "$root\src\dinput8.def") + $modObj + $sdkObj + $mhObj + $resObj + @('-lkernel32', '-luser32'))
if ($v32) {
    # Finishing step: no debug sections / symbol table, a valid checksum (see tools\harden_pe.py).
    Invoke-Checked 'tools\harden_pe.py' { python -I -B (Join-Path $root 'tools\harden_pe.py') "$out\dinput8.dll" }
}
Write-Host "Built $out\dinput8.dll"

# Body Swap add-on: a Dishonored Mod Loader plugin. Older versions: zig, as released.
if (-not (Test-Path "$root\bodyswap\BodySwap.rc")) {
    Invoke-Zig @('c++', '-target', 'x86-windows-gnu', '-std=c++20', '-O2', '-shared', '-o', "$out\BodySwap.dll",
                  "$root\bodyswap\bodyswap.cpp", "$root\bodyswap\BodySwap.def", '-lkernel32')
    Write-Host "Built $out\BodySwap.dll (older version: zig)"
    if ($Install) {
        Copy-Item "$out\dinput8.dll" "$Game\dinput8.dll" -Force
        New-Item -ItemType Directory -Force "$Game\Mods\plugins" | Out-Null
        Copy-Item "$out\BodySwap.dll" "$Game\Mods\plugins\BodySwap.dll" -Force
        Write-Host "Installed to $Game (the game must be closed)"
    }
    return
}
# 3.2 and later: MSVC (x86, static CRT, deterministic).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
$vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat' }
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    throw 'BodySwap.dll needs Visual Studio 2022 or its Build Tools with "Desktop development with C++" (MSVC x86 tools).'
}
New-Item -ItemType Directory -Force "$out\bodyswap" | Out-Null
$msvc = @(
    "@call `"$vcvars`" x86 >nul 2>&1 || exit /b 1",
    "cd /d `"$root\bodyswap`"",
    "rc /nologo /fo `"$out\bodyswap\BodySwap.res`" BodySwap.rc || exit /b 1",
    "cl /nologo /O2 /GS /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /Brepro /LD /Fo`"$out\bodyswap\\`" bodyswap.cpp `"$out\bodyswap\BodySwap.res`" /link /DEF:BodySwap.def /RELEASE /DYNAMICBASE /NXCOMPAT /Brepro /OUT:`"$out\BodySwap.dll`" /IMPLIB:`"$out\bodyswap\BodySwap.lib`" || exit /b 1"
)
Set-Content -Path "$out\bodyswap\build_msvc.cmd" -Value $msvc -Encoding ascii
Invoke-Checked 'MSVC BodySwap.dll' { cmd /c "`"$out\bodyswap\build_msvc.cmd`"" }
Write-Host "Built $out\BodySwap.dll"

if ($Install) {
    Copy-Item "$out\dinput8.dll" "$Game\dinput8.dll" -Force
    New-Item -ItemType Directory -Force "$Game\Mods\plugins" | Out-Null
    Copy-Item "$out\BodySwap.dll" "$Game\Mods\plugins\BodySwap.dll" -Force
    Write-Host "Installed to $Game (the game must be closed)"
}
