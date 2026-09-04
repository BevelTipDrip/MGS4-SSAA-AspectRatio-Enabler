# Generates PF Companion's icon and banner from code, so the art has no external source
# and can be regenerated after a rename or a palette change.
#
#   asi\res\PFCompanion.ico     16/32/48 as DIB entries, 256 as PNG
#   tool\res\PFCompanion.ico    same file
#   tool\res\banner.png         700x100, the settings tool's header
#
# Run from anywhere: paths are relative to this script.

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Palette: slate ground, brass accent, bone text.
$ground  = [System.Drawing.Color]::FromArgb(255, 26, 30, 36)
$groundHi= [System.Drawing.Color]::FromArgb(255, 40, 46, 55)
$brass   = [System.Drawing.Color]::FromArgb(255, 214, 160, 62)
$bone    = [System.Drawing.Color]::FromArgb(255, 232, 226, 214)
$muted   = [System.Drawing.Color]::FromArgb(255, 150, 156, 166)

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# The mark: a rounded slate tile, a brass orbit ring open at the top right (the companion),
# and a bone "PF" set in the tile. Drawn at any size from proportions.
function Draw-Mark([System.Drawing.Graphics]$g, [float]$x, [float]$y, [float]$size) {
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.PixelOffsetMode = 'HighQuality'

    $tile = New-RoundedRect $x $y $size $size ($size * 0.22)
    $tileBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF([float]$x, [float]$y)),
        (New-Object System.Drawing.PointF([float]$x, [float]($y + $size))),
        $groundHi, $ground)
    $g.FillPath($tileBrush, $tile)

    # Orbit ring, 300 degrees, gap at the top right where the companion dot sits.
    $inset = $size * 0.16
    $ringPen = New-Object System.Drawing.Pen($brass, [Math]::Max(1.0, $size * 0.075))
    $ringPen.StartCap = 'Round'; $ringPen.EndCap = 'Round'
    $g.DrawArc($ringPen, [float]($x + $inset), [float]($y + $inset), [float]($size - 2 * $inset), [float]($size - 2 * $inset), [float]-30, [float]300)

    # The companion: a dot in the ring's gap.
    $cx = $x + $size / 2; $cy = $y + $size / 2; $rad = ($size - 2 * $inset) / 2
    $ang = -60 * [Math]::PI / 180
    $dx = $cx + $rad * [Math]::Cos($ang); $dy = $cy + $rad * [Math]::Sin($ang)
    $dot = $size * 0.09
    $g.FillEllipse((New-Object System.Drawing.SolidBrush($bone)), [float]($dx - $dot), [float]($dy - $dot), [float]($dot * 2), [float]($dot * 2))

    # Monogram.
    $fontSize = $size * 0.36
    $font = New-Object System.Drawing.Font('Bahnschrift', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
    $rect = New-Object System.Drawing.RectangleF([float]$x, [float]($y + $size * 0.02), [float]$size, [float]$size)
    $g.DrawString('PF', $font, (New-Object System.Drawing.SolidBrush($bone)), $rect, $fmt)
}

function New-MarkBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    Draw-Mark $g 0 0 $size
    $g.Dispose()
    return $bmp
}

# --- ICO -------------------------------------------------------------------------------
# Entries 16..48 are stored as 32-bit DIBs (BITMAPINFOHEADER + bottom-up BGRA + AND mask),
# which every Windows shell version renders; 256 is a PNG entry. Note the BinaryWriter
# overloads: Write([byte[]]) must be called with a typed array or PowerShell picks the
# wrong overload and writes the array's length.
function Write-Ico([string]$path, [int[]]$sizes) {
    $entries = @()
    foreach ($s in $sizes) {
        $bmp = New-MarkBitmap $s
        if ($s -ge 256) {
            $ms = New-Object System.IO.MemoryStream
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $entries += [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
        }
        else {
            $stride = $s * 4
            $pixels = New-Object byte[] ($stride * $s)
            for ($y = 0; $y -lt $s; $y++) {
                for ($x = 0; $x -lt $s; $x++) {
                    $c = $bmp.GetPixel($x, $s - 1 - $y)
                    $o = $y * $stride + $x * 4
                    $pixels[$o] = $c.B; $pixels[$o + 1] = $c.G; $pixels[$o + 2] = $c.R; $pixels[$o + 3] = $c.A
                }
            }
            $maskStride = [int](([Math]::Ceiling($s / 32.0)) * 4)
            $mask = New-Object byte[] ($maskStride * $s)   # all opaque; alpha carries transparency
            $hdr = New-Object System.IO.MemoryStream
            $w = New-Object System.IO.BinaryWriter($hdr)
            $w.Write([int32]40); $w.Write([int32]$s); $w.Write([int32]($s * 2))
            $w.Write([int16]1); $w.Write([int16]32); $w.Write([int32]0)
            $w.Write([int32]($pixels.Length + $mask.Length))
            $w.Write([int32]0); $w.Write([int32]0); $w.Write([int32]0); $w.Write([int32]0)
            $w.Write([byte[]]$pixels); $w.Write([byte[]]$mask)
            $w.Flush()
            $entries += [pscustomobject]@{ Size = $s; Data = $hdr.ToArray() }
        }
        $bmp.Dispose()
    }

    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($out)
    $w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$entries.Count)
    $offset = 6 + 16 * $entries.Count
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([int16]1); $w.Write([int16]32)
        $w.Write([int32]$e.Data.Length); $w.Write([int32]$offset)
        $offset += $e.Data.Length
    }
    foreach ($e in $entries) { $w.Write([byte[]]$e.Data) }
    $w.Flush()
    [System.IO.File]::WriteAllBytes($path, $out.ToArray())
}

# --- Banner ----------------------------------------------------------------------------
function Write-Banner([string]$path) {
    $W = 700; $H = 100
    $bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'ClearTypeGridFit'
    $g.PixelOffsetMode = 'HighQuality'

    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF([float]0, [float]0)), (New-Object System.Drawing.PointF([float]$W, [float]0)), $groundHi, $ground)
    $g.FillRectangle($bg, 0, 0, $W, $H)

    # The brass rule under the banner is drawn by the tool (ui.cpp, kBannerRule) so it
    # spans the whole window; the PNG's right edge fades to the strip's ground colour.
    Draw-Mark $g 18 14 72

    $title = New-Object System.Drawing.Font('Bahnschrift', 34, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $sub = New-Object System.Drawing.Font('Segoe UI', 14, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $g.DrawString('PF COMPANION', $title, (New-Object System.Drawing.SolidBrush($bone)), 108, 20)
    $g.DrawString('Graphics settings for METAL GEAR SOLID 4', $sub, (New-Object System.Drawing.SolidBrush($muted)), 110, 62)

    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$asiRes = Join-Path $root 'asi\res'
$toolRes = Join-Path $root 'tool\res'
New-Item -ItemType Directory -Force $asiRes | Out-Null
New-Item -ItemType Directory -Force $toolRes | Out-Null

Write-Ico (Join-Path $asiRes 'PFCompanion.ico') @(16, 32, 48, 256)
Copy-Item (Join-Path $asiRes 'PFCompanion.ico') (Join-Path $toolRes 'PFCompanion.ico') -Force
Write-Banner (Join-Path $toolRes 'banner.png')
Write-Host "Wrote $asiRes\PFCompanion.ico, $toolRes\PFCompanion.ico and $toolRes\banner.png"
