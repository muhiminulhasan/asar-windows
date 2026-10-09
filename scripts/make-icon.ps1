# make-icon.ps1 - generates resources/asar.ico (multi-size PNG-compressed ICO).
# Only standard .NET (System.Drawing) is used so the script runs anywhere.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot   # repo root
$out = Join-Path $root 'resources\asar.ico'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'resources') | Out-Null

$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$pngs = @()

foreach ($size in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded amber box with a subtle vertical gradient.
    $rect = New-Object System.Drawing.RectangleF(1, 1, ($size - 2), ($size - 2))
    $radius = [Math]::Max(3, [int]($size * 0.18))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 245, 158, 11)   # amber
    $c2 = [System.Drawing.Color]::FromArgb(255, 217, 119, 6)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 90)
    $g.FillPath($brush, $path)

    # White "A" glyph (archive monogram).
    $fontSize = [Math]::Max(9.0, $size * 0.62)
    $font = New-Object System.Drawing.Font('Segoe UI', [float]$fontSize,
        [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textRect = New-Object System.Drawing.RectangleF(0, -($size * 0.03), $size, $size)
    $g.DrawString('A', $font, [System.Drawing.Brushes]::White, $textRect, $sf)

    # Extract-arrow chevron along the bottom for larger sizes.
    if ($size -ge 32) {
        $penW = [Math]::Max(1.5, $size * 0.06)
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(230, 255, 255, 255), $penW)
        $pen.StartCap = 'Round'
        $pen.EndCap = 'Round'
        $cx = $size / 2
        $y0 = $size * 0.78
        $w = $size * 0.16
        $g.DrawLine($pen, $cx - $w, $y0, $cx, $y0 + $w * 0.8)
        $g.DrawLine($pen, $cx, $y0 + $w * 0.8, $cx + $w, $y0)
    }

    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , @{ Size = $size; Bytes = $ms.ToArray() }
    $bmp.Dispose()
}

# Assemble the ICO container (PNG-compressed entries, valid since Vista).
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([uint16]0)               # reserved
$bw.Write([uint16]1)               # type: icon
$bw.Write([uint16]$pngs.Count)

$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $s = if ($p.Size -ge 256) { 0 } else { $p.Size }
    $bw.Write([byte]$s)            # width
    $bw.Write([byte]$s)            # height
    $bw.Write([byte]0)             # palette
    $bw.Write([byte]0)             # reserved
    $bw.Write([uint16]1)           # planes
    $bw.Write([uint16]32)          # bpp
    $bw.Write([uint32]$p.Bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $p.Bytes.Length
}
foreach ($p in $pngs) {
    $bw.Write($p.Bytes)
}
$bw.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
Write-Host "wrote $out ($($ms.Length) bytes, $($pngs.Count) sizes)"
