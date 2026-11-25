param(
    [string]$BuildDir = "build",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$Test
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Args = @(
    "-S", $RepoRoot,
    "-B", (Join-Path $PSScriptRoot $BuildDir),
    "-DDESKMATE_BUILD_WINDOWS_HOST=ON",
    "-DDESKMATE_BUILD_PLATFORM_TESTS=ON"
)
if ($VcpkgRoot) {
    $Args += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake"
}
cmake @Args
cmake --build (Join-Path $PSScriptRoot $BuildDir) --config Release

if ($Test) {
    ctest --test-dir (Join-Path $PSScriptRoot $BuildDir) -C Release --output-on-failure
}
