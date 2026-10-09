# install.ps1 - installs asar-windows for the current user (no admin needed).
#
# - Copies asarshell.dll / asarview.exe / asar.exe / asar.ico to
#   %LOCALAPPDATA%\AsarTools and registers the Explorer context menu (HKCU),
#   using the build that matches the OS architecture (x64 / Win32 / ARM64).
# - Copies 7z-asar.dll into 7-Zip's Formats folder, automatically matching the
#   architecture of the installed 7-Zip (read from 7z.exe's PE header).
#   That step needs write access to the 7-Zip install directory (run an
#   elevated shell, or use a per-user 7-Zip).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
#   powershell ... install.ps1 -Arch x64 -SevenZipPath "D:\Tools\7-Zip" -Skip7Zip -SkipShell

param(
    [string]$BuildDir = "",
    [string]$SevenZipPath = "",
    [ValidateSet("", "x64", "Win32", "ARM64")]
    [string]$Arch = "",
    [switch]$Skip7Zip,
    [switch]$SkipShell,
    [switch]$RestartExplorer
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# Locate the newest Release build outputs.
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
if (-not (Test-Path $BuildDir)) {
    Write-Error "No build found at $BuildDir - run scripts\build.cmd first."
}

# OS architecture -> build folder name.
$osArch = switch ($env:PROCESSOR_ARCHITECTURE) {
    'AMD64' { 'x64' }
    'x86'   { 'Win32' }
    'ARM64' { 'ARM64' }
    default { 'x64' }
}
if (-not $Arch) { $Arch = $osArch }

# Reads the PE machine field (IMAGE_FILE_HEADER.Machine) of an executable.
function Get-PEMachine($exePath) {
    $fs = [System.IO.File]::OpenRead($exePath)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C                 # e_lfanew
        $peOffset = $br.ReadInt32()
        $fs.Position = $peOffset + 4        # skip 'PE\0\0'
        return $br.ReadUInt16()
    } finally {
        $fs.Dispose()
    }
}

function Find-Output($name, $arch) {
    # Prefer build\<arch>, fall back to any architecture with a warning.
    $roots = @((Join-Path $BuildDir $arch), $BuildDir)
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) {
            if ($root -eq $BuildDir) {
                Write-Warning "$name found outside build\<arch> - architecture may not match."
            }
            return $hit.FullName
        }
    }
    Write-Error "$name not found under $BuildDir (built for $arch?)"
}

# ---------------------------------------------------------------- shell side
$replacedInUse = $false
if (-not $SkipShell) {
    $dest = Join-Path $env:LOCALAPPDATA 'AsarTools'
    New-Item -ItemType Directory -Force -Path $dest | Out-Null

    $regsrv32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'

    # Unregister first so Explorer is asked to release the old DLLs before we
    # overwrite them.
    foreach ($f in @('asarshell.dll', 'asarfolder.dll')) {
        $existing = Join-Path $dest $f
        if (Test-Path $existing) {
            Start-Process -FilePath $regsrv32 -ArgumentList "/s", "/u", "`"$existing`"" -Wait -PassThru | Out-Null
        }
    }
    Start-Sleep -Milliseconds 500

    foreach ($f in @('asarshell.dll', 'asarfolder.dll', 'asarview.exe', 'asar.exe')) {
        $target = Join-Path $dest $f
        try {
            Copy-Item (Find-Output $f $Arch) $target -Force
        } catch {
            # The DLL may still be locked (Explorer unloads lazily). Windows
            # allows renaming in-use files - move the old copy aside and
            # retry; Explorer finishes loading the NEW file next restart.
            $old = "$target.old"
            Remove-Item $old -Force -ErrorAction SilentlyContinue
            Move-Item $target $old -Force
            Copy-Item (Find-Output $f $Arch) $target -Force
            $script:replacedInUse = $true
        }
    }
    if ($Arch -ne $osArch) {
        Write-Warning "Installing $Arch binaries on an $osArch OS - Explorer needs native binaries!"
    }
    Copy-Item (Join-Path $repoRoot 'resources\asar.ico') $dest -Force

    # Self-registration (both DLLs write only to HKCU - no admin needed).
    $proc = Start-Process -FilePath $regsrv32 -ArgumentList "/s", "`"$dest\asarshell.dll`"" -Wait -PassThru
    if ($proc.ExitCode -ne 0) { Write-Error "regsvr32 failed (exit $($proc.ExitCode))" }
    $proc = Start-Process -FilePath $regsrv32 -ArgumentList "/s", "`"$dest\asarfolder.dll`"" -Wait -PassThru
    if ($proc.ExitCode -ne 0) { Write-Error "regsvr32 for asarfolder failed (exit $($proc.ExitCode))" }

    # If a DLL was locked (older copy still mapped in Explorer), the freshly
    # installed one only activates after the shell restarts - do it now so
    # the first double-click after this install works.
    if ($script:replacedInUse -or $RestartExplorer) {
        Write-Host "Restarting Explorer to load the updated extension..."
        Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 800
        Start-Process explorer.exe
        Start-Sleep -Milliseconds 500
    }

    Write-Host "Explorer integration installed to $dest (per-user, $Arch)."
    Write-Host "  - double-click a .asar: browse it inside Explorer"
    Write-Host "  - right-click a .asar:  extract / open / properties"
    Write-Host "  - right-click a folder: pack to .asar"
}

# ------------------------------------------------------------- 7-Zip side
if (-not $Skip7Zip) {
    if (-not $SevenZipPath) {
        $candidates = @("$env:ProgramFiles\7-Zip", "${env:ProgramFiles(x86)}\7-Zip")
        foreach ($c in $candidates) {
            if (Test-Path (Join-Path $c '7z.exe')) { $SevenZipPath = $c; break }
        }
    }
    if (-not $SevenZipPath -or -not (Test-Path (Join-Path $SevenZipPath '7z.exe'))) {
        Write-Warning "7-Zip installation not found; skipping plugin install (use -SevenZipPath)."
    } else {
        # Read the PE header of 7z.exe to learn which architecture this
        # 7-Zip edition is, so the plugin DLL matches it exactly.
        $machine = Get-PEMachine (Join-Path $SevenZipPath '7z.exe')
        $sevenZipArch = switch ($machine) {
            0x8664  { 'x64' }
            0x014C  { 'Win32' }
            0xAA64  { 'ARM64' }
            default { '' }
        }
        $wantArch = if ($Arch) { $Arch } else { $sevenZipArch }
        if ($sevenZipArch -and ($wantArch -ne $sevenZipArch)) {
            Write-Warning "Installed 7-Zip is $sevenZipArch but -Arch $Arch was requested; using $wantArch anyway."
        }
        $pluginDll = Find-Output '7z-asar.dll' $wantArch

        $formats = Join-Path $SevenZipPath 'Formats'
        try {
            New-Item -ItemType Directory -Force -Path $formats | Out-Null
            Copy-Item $pluginDll $formats -Force
            Write-Host "7-Zip plugin installed ($wantArch): $formats\7z-asar.dll"
        } catch {
            Write-Warning "Could not copy the plugin into $formats ($($_.Exception.Message))."
            Write-Warning "Run this script from an elevated shell, or copy 7z-asar.dll manually."
        }
    }
}

Write-Host ""
Write-Host "Done. Uninstall with scripts\uninstall.ps1"

exit 0
