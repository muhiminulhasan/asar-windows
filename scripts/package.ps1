# package.ps1 - stages per-architecture release packages and zips them.
#
# Layout produced under release\<version>\:
#   <arch>\7z-asar.dll                      -> 7z-asar-<ver>-<arch>.zip
#   <arch>\asarshell.dll, asarfolder.dll,
#          asarview.exe, asar.exe, asar.ico -> asar-tools-<ver>-<arch>.zip
#   <arch>\asar.dll, asar.lib, asar_c.h     -> asar-lib-<ver>-<arch>.zip
#
# Usage:
#   scripts\package.ps1 [-Arch x64|Win32|ARM64|all] [-Version 0.1.0] [-BuildDir path]

param(
    [string]$Arch = "all",
    [string]$Version = "0.1.0",
    [string]$BuildDir = ""
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
if (-not (Test-Path $BuildDir)) { Write-Error "no build at $BuildDir - run scripts\build.cmd first" }

$osArch = switch ($env:PROCESSOR_ARCHITECTURE) {
    'AMD64' { 'x64' }
    'x86'   { 'Win32' }
    'ARM64' { 'ARM64' }
    default { 'x64' }
}
if (-not $Arch) { $Arch = 'all' }

$archs = if ($Arch -eq 'all') { @('x64', 'Win32', 'ARM64') } else { @($Arch) }
$releaseRoot = Join-Path $repoRoot "release\$Version"

function Find-Output($name, $arch) {
    $roots = @((Join-Path $BuildDir $arch), $BuildDir)
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    Write-Error "$name not found under $BuildDir (arch $arch)"
}

foreach ($a in $archs) {
    Write-Host "packaging $a..."
    $stage = Join-Path $releaseRoot $a
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    # 1. 7-Zip plugin.
    $pluginDir = Join-Path $stage '7z-plugin'
    New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
    Copy-Item (Find-Output '7z-asar.dll' $a) $pluginDir -Force

    # 2. Explorer integration bundle.
    $toolsDir = Join-Path $stage 'explorer-integration'
    New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
    foreach ($f in @('asarshell.dll', 'asarfolder.dll', 'asarview.exe', 'asar.exe')) {
        Copy-Item (Find-Output $f $a) $toolsDir -Force
    }
    Copy-Item (Join-Path $repoRoot 'resources\asar.ico') $toolsDir -Force

    # 3. Developer library (DLL + import lib + C header).
    $libDir = Join-Path $stage 'asar-lib'
    New-Item -ItemType Directory -Force -Path $libDir | Out-Null
    Copy-Item (Find-Output 'asar.dll' $a) $libDir -Force
    $implib = Find-Output 'asar.lib' $a
    if ($implib) { Copy-Item $implib $libDir -Force }
    Copy-Item (Join-Path $repoRoot 'libasar\include\asar\asar_c.h') $libDir -Force

    # Zip each package.
    foreach ($pkg in @('7z-plugin', 'explorer-integration', 'asar-lib')) {
        $src = Join-Path $stage $pkg
        $zip = Join-Path $releaseRoot "$pkg-$Version-$a.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Compress-Archive -Path (Join-Path $src '*') -DestinationPath $zip -Force
        Write-Host ("  {0}" -f $zip)
    }
}

Write-Host ""
Write-Host "Packages in $releaseRoot"
