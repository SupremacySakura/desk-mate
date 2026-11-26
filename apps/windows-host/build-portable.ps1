# Builds DeskMateHost.exe without Visual Studio or CMake.
#
# Uses a portable llvm-mingw toolchain (clang + lld + Windows SDK headers +
# UCRT) plus a libusb source tree. Both are plain archives, so a machine with
# no developer tools installed can still produce the host binary.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-portable.ps1 `
#     -ToolchainDir C:\dm\llvm-mingw-20261117-ucrt-x86_64 `
#     -LibusbSource C:\dm\libusb-1.0.30 `
#     -OutputDir    C:\dm\dist\windows
param(
    [Parameter(Mandatory = $true)][string]$ToolchainDir,
    [Parameter(Mandatory = $true)][string]$LibusbSource,
    [string]$OutputDir = "",
    [string]$WorkDir = "",
    [string]$ObjDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $OutputDir) { $OutputDir = Join-Path $RepoRoot "dist\windows" }
if (-not $WorkDir) { $WorkDir = Join-Path $PSScriptRoot "build\portable-work" }
$Bin = Join-Path $ToolchainDir "bin"
$Clang = Join-Path $Bin "clang.exe"
$ClangPP = Join-Path $Bin "clang++.exe"
$Ar = Join-Path $Bin "llvm-ar.exe"
foreach ($tool in @($Clang, $ClangPP, $Ar)) {
    if (-not (Test-Path $tool)) { throw "missing toolchain binary: $tool" }
}
$Windres = $null
foreach ($candidate in @("llvm-windres.exe", "x86_64-w64-mingw32-windres.exe", "windres.exe")) {
    $p = Join-Path $Bin $candidate
    if (Test-Path $p) { $Windres = $p; break }
}
if (-not (Test-Path (Join-Path $LibusbSource "libusb\core.c"))) {
    throw "LibusbSource must point at an extracted libusb-1.0.x tree"
}

if (-not $ObjDir) { $ObjDir = Join-Path $WorkDir "obj" }
New-Item -ItemType Directory -Force -Path $OutputDir, $WorkDir, $ObjDir | Out-Null

# libusb expects a configure-generated config.h. Only the Windows-relevant
# defines are needed; the POSIX ones stay undefined.
$LibusbCfg = Join-Path $WorkDir "libusb-cfg"
New-Item -ItemType Directory -Force -Path $LibusbCfg | Out-Null
@'
#ifndef DESKMATE_LIBUSB_CONFIG_H
#define DESKMATE_LIBUSB_CONFIG_H
#define PLATFORM_WINDOWS 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define ENABLE_LOGGING 1
#define DEFAULT_VISIBILITY
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#endif
'@ | Set-Content -Path (Join-Path $LibusbCfg "config.h") -Encoding ASCII

# The host includes <libusb-1.0/libusb.h> (vcpkg layout); the source tree keeps
# the header at libusb/libusb.h. Mirror it instead of patching the source.
$Shim = Join-Path $WorkDir "include\libusb-1.0"
New-Item -ItemType Directory -Force -Path $Shim | Out-Null
Copy-Item (Join-Path $LibusbSource "libusb\libusb.h") (Join-Path $Shim "libusb.h") -Force

Write-Host "==> libusb"
$LibusbOut = Join-Path $WorkDir "libusb"
New-Item -ItemType Directory -Force -Path $LibusbOut | Out-Null
$LibusbSources = @(
    "libusb\core.c", "libusb\descriptor.c", "libusb\hotplug.c", "libusb\io.c",
    "libusb\strerror.c", "libusb\sync.c",
    "libusb\os\windows_common.c", "libusb\os\windows_usbdk.c",
    "libusb\os\windows_winusb.c", "libusb\os\events_windows.c",
    "libusb\os\threads_windows.c"
)
$LibusbObjs = @()
foreach ($rel in $LibusbSources) {
    $obj = Join-Path $LibusbOut ("{0}.o" -f [IO.Path]::GetFileNameWithoutExtension($rel))
    & $Clang -std=c11 -O2 -DHAVE_CONFIG_H -I $LibusbCfg -I (Join-Path $LibusbSource "libusb") `
        -c (Join-Path $LibusbSource $rel) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "libusb compile failed: $rel" }
    $LibusbObjs += $obj
}
$LibusbLib = Join-Path $WorkDir "libusb.a"
& $Ar rcs $LibusbLib @LibusbObjs
if ($LASTEXITCODE -ne 0) { throw "llvm-ar failed" }

Write-Host "==> DeskMate host"
$Includes = @(
    "-I", (Join-Path $RepoRoot "core\include"),
    "-I", (Join-Path $RepoRoot "apps\common\include"),
    "-I", (Join-Path $RepoRoot "apps\windows-host\include"),
    "-I", (Join-Path $WorkDir "include")
)
# _WIN32_WINNT gates DXGI Desktop Duplication, Media Foundation and
# SetProcessDpiAwarenessContext.
$Defines = @(
    "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
    "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-DDESKMATE_HAVE_LIBUSB=1"
)
$Sources = @()
foreach ($dir in @("core\src", "apps\common\src", "apps\windows-host\src")) {
    $Sources += Get-ChildItem (Join-Path $RepoRoot $dir) -Filter *.cpp | ForEach-Object { $_.FullName }
}
$Objs = @()
foreach ($src in $Sources) {
    $obj = Join-Path $ObjDir ("{0}.o" -f [IO.Path]::GetFileNameWithoutExtension($src))
    & $ClangPP -std=c++20 -O2 -c $src -o $obj @Includes @Defines
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    $Objs += $obj
}

$ResObjs = @()
$RcPath = Join-Path $RepoRoot "apps\windows-host\resources\DeskMateHost.rc"
if ($Windres -and (Test-Path $RcPath)) {
    Write-Host "==> Windows icon & version resource"
    $ResObj = Join-Path $ObjDir "DeskMateHost.res.o"
    & $Windres -O coff -I (Join-Path $RepoRoot "apps\windows-host\resources") `
        -I (Join-Path $RepoRoot "apps\windows-host\include") $RcPath $ResObj
    if ($LASTEXITCODE -ne 0) { throw "windres failed: $RcPath" }
    $ResObjs += $ResObj
}

$Exe = Join-Path $OutputDir "DeskMateHost.exe"
# Windows GUI subsystem (WinMain): attaches console automatically for --headless/--list-monitors.
& $ClangPP -std=c++20 -O2 -mwindows -o $Exe @Objs @ResObjs $LibusbLib `
    -ld2d1 -ldwrite -ldwmapi `
    -ld3d11 -ldxgi -ld3dcompiler -ldxguid `
    -lmfplat -lmfreadwrite -lmfuuid -lwmcodecdspuuid `
    -lshcore -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lavrt -lwinhttp `
    -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lsetupapi -lcfgmgr32 -static
if ($LASTEXITCODE -ne 0) { throw "link failed" }

$size = [math]::Round((Get-Item $Exe).Length / 1MB, 2)
Write-Host ""
Write-Host "DeskMateHost.exe -> $Exe ($size MB)"
