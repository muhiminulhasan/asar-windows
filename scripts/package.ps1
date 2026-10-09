# package.ps1 - stages per-architecture release packages and zips them.
#
# Packages (each also produced as a separate <name>-<ver>-<arch>.zip):
#   7z-plugin    7z-asar.dll                    7-Zip format plugin
#   asarfolder   asarfolder.dll + asar.ico      standalone in-Explorer
#                                               browsing (regsvr32 to use)
#   explorer-tools asarshell.dll + asarview.exe + asar.exe + asar.ico
#                context menu (extract/pack/open) + viewer + CLI
#   asar-lib     asar.dll + asar.lib + asar_c.h  developer C API
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

if (-not $Arch) { $Arch = 'all' }
$archs = if ($Arch -eq 'all') { @('x64', 'Win32', 'ARM64') } else { @($Arch) }
$releaseRoot = Join-Path $repoRoot "release\$Version"

$installExplorer = @'
ASAR Tools - Explorer integration (context menu, viewer, CLI)

INSTALL (per-user, no admin)
  1. regsvr32 asarshell.dll
  2. regsvr32 asarfolder.dll   <- enables double-click browsing of .asar
explode
  3. asarview.exe              <- viewer; asar.exe is the command line

UNINSTALL
  regsvr32 /u asarshell.dll
  regsvr32 /u asarfolder.dll
  then delete this folder.

Notes
  - asarfolder.dll alone gives in-Explorer .asar browsing; asarshell.dll
    alone gives right-click Extract/Pack. Install both for everything.
'@

$installFolder = @'
ASAR folder plugin - browse .asar archives inside Windows Explorer

INSTALL (per-user, no admin)
  regsvr32 asarfolder.dll
  then open any .asar: Explorer navigates into it like a .zip.

UNINSTALL
  regsvr32 /u asarfolder.dll

For right-click Extract/Pack and the viewer, install the
explorer-integration package too.
'@

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

    # 1. 7-Zip plugin.
    $pluginDir = Join-Path $stage '7z-plugin'
    New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
    Copy-Item (Find-Output '7z-asar.dll' $a) $pluginDir -Force

    # 2. asarfolder - standalone in-Explorer browsing.
    $folderDir = Join-Path $stage 'asarfolder'
    New-Item -ItemType Directory -Force -Path $folderDir | Out-Null
    Copy-Item (Find-Output 'asarfolder.dll' $a) $folderDir -Force
    Copy-Item (Join-Path $repoRoot 'resources\asar.ico') $folderDir -Force
    Set-Content -Path (Join-Path $folderDir 'INSTALL.txt') -Value $installFolder -Encoding Unicode

    # 3. Explorer tools (context menu + viewer + CLI; asarfolder optional).
    $toolsDir = Join-Path $stage 'explorer-tools'
    New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null
    foreach ($f in @('asarshell.dll', 'asarview.exe', 'asar.exe')) {
        Copy-Item (Find-Output $f $a) $toolsDir -Force
    }
    Copy-Item (Join-Path $repoRoot 'resources\asar.ico') $toolsDir -Force
    Set-Content -Path (Join-Path $toolsDir 'INSTALL.txt') -Value $installExplorer -Encoding Unicode

    # 4. Developer library (DLL + import lib + C header).
    $libDir = Join-Path $stage 'asar-lib'
    New-Item -ItemType Directory -Force -Path $libDir | Out-Null
    Copy-Item (Find-Output 'asar.dll' $a) $libDir -Force
    $implib = Find-Output 'asar.lib' $a
    if ($implib) { Copy-Item $implib $libDir -Force }
    Copy-Item (Join-Path $repoRoot 'libasar\include\asar\asar_c.h') $libDir -Force

    # Zip each package.
    foreach ($pkg in @('7z-plugin', 'asarfolder', 'explorer-tools', 'asar-lib')) {
        $src = Join-Path $stage $pkg
        $zip = Join-Path $releaseRoot "$pkg-$Version-$a.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Compress-Archive -Path (Join-Path $src '*') -DestinationPath $zip -Force
        Write-Host ("  {0}" -f $zip)
    }
}

Write-Host ""
Write-Host "Packages in $releaseRoot"
