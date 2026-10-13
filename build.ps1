# Builds CorvoBody (dinput8.dll) and the Body Swap add-on (BodySwap.dll) for Dishonored (32-bit) with Zig.
#
# Usage:   powershell -ExecutionPolicy Bypass -File build.ps1 [-Zig path\to\zig.exe] [-Install] [-Game folder]
# Needs:   Git and Zig 0.17.0 (https://ziglang.org/download/ - or: winget install zig.zig).
# Output:  build\dinput8.dll and build\BodySwap.dll
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
Invoke-Zig (@('c++', '-target', 'x86-windows-gnu', '-shared', '-o', "$out\dinput8.dll",
              "$root\src\dinput8.def") + $modObj + $sdkObj + $mhObj + @('-lkernel32', '-luser32'))
Write-Host "Built $out\dinput8.dll"

# Body Swap add-on: a Dishonored Mod Loader plugin.
Invoke-Zig @('c++', '-target', 'x86-windows-gnu', '-std=c++20', '-O2', '-shared', '-o', "$out\BodySwap.dll",
              "$root\bodyswap\bodyswap.cpp", "$root\bodyswap\BodySwap.def", '-lkernel32')
Write-Host "Built $out\BodySwap.dll"

if ($Install) {
    Copy-Item "$out\dinput8.dll" "$Game\dinput8.dll" -Force
    New-Item -ItemType Directory -Force "$Game\Mods\plugins" | Out-Null
    Copy-Item "$out\BodySwap.dll" "$Game\Mods\plugins\BodySwap.dll" -Force
    Write-Host "Installed to $Game (the game must be closed)"
}
