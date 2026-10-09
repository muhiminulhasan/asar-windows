# uninstall.ps1 - removes the per-user Explorer integration and the 7-Zip plugin.
param(
    [string]$SevenZipPath = "",
    [switch]$Skip7Zip,
    [switch]$SkipShell
)

$ErrorActionPreference = 'Stop'

if (-not $SkipShell) {
    $dest = Join-Path $env:LOCALAPPDATA 'AsarTools'
    $regsrv32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
    $dll = Join-Path $dest 'asarshell.dll'
    if (Test-Path $dll) {
        & $regsrv32 /s /u $dll
        Write-Host "Explorer context menu unregistered."
    }
    $folderDll = Join-Path $dest 'asarfolder.dll'
    if (Test-Path $folderDll) {
        & $regsrv32 /s /u $folderDll
        Write-Host "Archive browsing unregistered."
    }
    if (Test-Path $dest) {
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not $Skip7Zip) {
    if (-not $SevenZipPath) {
        $candidates = @("$env:ProgramFiles\7-Zip", "${env:ProgramFiles(x86)}\7-Zip")
        foreach ($c in $candidates) {
            if (Test-Path (Join-Path $c '7z.exe')) { $SevenZipPath = $c; break }
        }
    }
    if ($SevenZipPath -and (Test-Path (Join-Path $SevenZipPath '7z.exe'))) {
        $plugin = Join-Path $SevenZipPath 'Formats\7z-asar.dll'
        if (Test-Path $plugin) {
            Remove-Item $plugin -Force
            Write-Host "7-Zip plugin removed: $plugin"
        }
    }
}

Write-Host "Done."
