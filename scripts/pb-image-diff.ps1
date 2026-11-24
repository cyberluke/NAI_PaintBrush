# pb-image-diff.ps1 — pixel-exact before/after comparison for Paintbrush gates
#
#   pwsh scripts/pb-image-diff.ps1 -Before <png> -After <png> `
#        [-Mask <png>] [-Rect "x,y,w,h"] [-Tolerance 0] [-DiffOut <png>] `
#        [-CropRect "x,y,w,h"] [-CropScale 3] [-CropPrefix <path>]
#
# Prints one JSON object:
#   width/height, changed, changedInside, changedOutside, maxDeltaOutside,
#   meanDeltaOutside, maxDeltaInside, meanDeltaInside, bbox of changed pixels,
#   maskCovered pixels, tolerance, and the optional crop paths.
#
# With -Mask, "inside" means mask gray > 127. With -Rect, "inside" means the
# rectangle. Without either, every pixel is inside and outside counts are 0.
# Pixels are compared in 32-bit BGRA after both images are normalised to
# Format32bppArgb, which is exactly what the Paintbrush renderer writes.

param(
    [Parameter(Mandatory = $true)][string]$Before,
    [Parameter(Mandatory = $true)][string]$After,
    [string]$Mask = '',
    [string]$Rect = '',
    [int]$Tolerance = 0,
    [string]$DiffOut = '',
    [string]$CropRect = '',
    [int]$CropScale = 3,
    [string]$CropPrefix = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Load-Bgra([string]$Path) {
    $source = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $Path).Path)
    try {
        $bmp = New-Object System.Drawing.Bitmap $source.Width, $source.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.DrawImage($source, 0, 0, $source.Width, $source.Height)
        $g.Dispose()
        $result = @{ Bitmap = $bmp; Width = $source.Width; Height = $source.Height }
        return [pscustomobject]$result
    } finally {
        $source.Dispose()
    }
}

function Read-BgraBytes($Loaded) {
    $rect = New-Object System.Drawing.Rectangle 0, 0, $Loaded.Width, $Loaded.Height
    $data = $Loaded.Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $stride = $data.Stride
        $bytes = New-Object byte[] ($stride * $Loaded.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        return [pscustomobject]@{ Bytes = $bytes; Stride = $stride }
    } finally {
        $Loaded.Bitmap.UnlockBits($data)
    }
}

$beforeImg = Load-Bgra $Before
$afterImg = Load-Bgra $After
if ($beforeImg.Width -ne $afterImg.Width -or $beforeImg.Height -ne $afterImg.Height) {
    throw "Image size mismatch: $($beforeImg.Width)x$($beforeImg.Height) vs $($afterImg.Width)x$($afterImg.Height)"
}
$w = $beforeImg.Width
$h = $beforeImg.Height
$beforeData = Read-BgraBytes $beforeImg
$afterData = Read-BgraBytes $afterImg

$inside = $null
if ($Mask) {
    $maskImg = Load-Bgra $Mask
    if ($maskImg.Width -ne $w -or $maskImg.Height -ne $h) {
        throw "Mask size mismatch: $($maskImg.Width)x$($maskImg.Height) vs ${w}x${h}"
    }
    $maskData = Read-BgraBytes $maskImg
    $inside = New-Object bool[] ($w * $h)
    for ($y = 0; $y -lt $h; $y++) {
        $row = $y * $maskData.Stride
        $base = $y * $w
        for ($x = 0; $x -lt $w; $x++) {
            $inside[$base + $x] = ($maskData.Bytes[$row + $x * 4] -ge 128)
        }
    }
    $maskImg.Bitmap.Dispose()
} elseif ($Rect) {
    $parts = $Rect.Split(',') | ForEach-Object { [int]$_.Trim() }
    if ($parts.Count -ne 4) { throw "Rect must be x,y,w,h" }
    $rx = [Math]::Max(0, $parts[0]); $ry = [Math]::Max(0, $parts[1])
    $rw = [Math]::Min($w - $rx, $parts[2]); $rh = [Math]::Min($h - $ry, $parts[3])
    $inside = New-Object bool[] ($w * $h)
    for ($y = $ry; $y -lt ($ry + $rh); $y++) {
        $base = $y * $w
        for ($x = $rx; $x -lt ($rx + $rw); $x++) { $inside[$base + $x] = $true }
    }
} else {
    $inside = New-Object bool[] ($w * $h)
    for ($i = 0; $i -lt $inside.Length; $i++) { $inside[$i] = $true }
}

$diffBmp = if ($DiffOut) {
    New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
} else { $null }
$diffData = if ($diffBmp) {
    $r = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $diffBmp.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
} else { $null }
$diffBytes = if ($diffData) { New-Object byte[] ($diffData.Stride * $h) } else { $null }

$changed = 0; $changedInside = 0; $changedOutside = 0
$sumIn = 0.0; $sumOut = 0.0; $maxIn = 0; $maxOut = 0
$countIn = 0; $countOut = 0
$minX = $w; $minY = $h; $maxX = -1; $maxY = -1
$maskCovered = 0

for ($y = 0; $y -lt $h; $y++) {
    $row = $y * $beforeData.Stride
    $drow = if ($diffBytes) { $y * $diffData.Stride } else { 0 }
    $base = $y * $w
    for ($x = 0; $x -lt $w; $x++) {
        $i = $row + $x * 4
        $db = [Math]::Abs([int]$beforeData.Bytes[$i] - [int]$afterData.Bytes[$i])
        $dg = [Math]::Abs([int]$beforeData.Bytes[$i + 1] - [int]$afterData.Bytes[$i + 1])
        $dr = [Math]::Abs([int]$beforeData.Bytes[$i + 2] - [int]$afterData.Bytes[$i + 2])
        $delta = [Math]::Max($db, [Math]::Max($dg, $dr))
        $isInside = $inside[$base + $x]
        if ($isInside) { $maskCovered++ }
        if ($delta -gt $Tolerance) {
            $changed++
            if ($isInside) { $changedInside++ } else { $changedOutside++ }
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
        if ($isInside) {
            $sumIn += $delta; $countIn++
            if ($delta -gt $maxIn) { $maxIn = $delta }
        } else {
            $sumOut += $delta; $countOut++
            if ($delta -gt $maxOut) { $maxOut = $delta }
        }
        if ($diffBytes) {
            $v = if ($delta -eq 0) { 0 } else { [Math]::Min(255, 40 + $delta * 3) }
            $diffBytes[$drow + $x * 4] = $v
            $diffBytes[$drow + $x * 4 + 1] = $v
            $diffBytes[$drow + $x * 4 + 2] = $v
            $diffBytes[$drow + $x * 4 + 3] = 255
        }
    }
}

if ($diffBmp) {
    [System.Runtime.InteropServices.Marshal]::Copy($diffBytes, 0, $diffData.Scan0, $diffBytes.Length)
    $diffBmp.UnlockBits($diffData)
    $diffBmp.Save($DiffOut, [System.Drawing.Imaging.ImageFormat]::Png)
    $diffBmp.Dispose()
}

$result = [ordered]@{
    before        = (Resolve-Path -LiteralPath $Before).Path
    after         = (Resolve-Path -LiteralPath $After).Path
    mask          = if ($Mask) { (Resolve-Path -LiteralPath $Mask).Path } else { $null }
    rect          = if ($Rect) { $Rect } else { $null }
    width         = $w
    height        = $h
    tolerance     = $Tolerance
    maskCovered   = $maskCovered
    changed       = $changed
    changedInside = $changedInside
    changedOutside = $changedOutside
    maxDeltaInside  = $maxIn
    meanDeltaInside = if ($countIn -gt 0) { [Math]::Round($sumIn / $countIn, 4) } else { 0 }
    maxDeltaOutside  = $maxOut
    meanDeltaOutside = if ($countOut -gt 0) { [Math]::Round($sumOut / $countOut, 4) } else { 0 }
    bbox          = if ($changed -gt 0) { "$minX,$minY,$($maxX - $minX + 1),$($maxY - $minY + 1)" } else { $null }
    diffOut       = if ($DiffOut) { (Resolve-Path -LiteralPath $DiffOut).Path } else { $null }
    cropPaths     = @()
}

if ($CropRect -and $CropPrefix) {
    $parts = $CropRect.Split(',') | ForEach-Object { [int]$_.Trim() }
    $cx = [Math]::Max(0, $parts[0]); $cy = [Math]::Max(0, $parts[1])
    $cw = [Math]::Min($w - $cx, $parts[2]); $ch = [Math]::Min($h - $cy, $parts[3])
    foreach ($pair in @(@('before', $Before), @('after', $After))) {
        $src = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $pair[1]).Path)
        try {
            $outW = $cw * $CropScale; $outH = $ch * $CropScale
            $out = New-Object System.Drawing.Bitmap $outW, $outH, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $g = [System.Drawing.Graphics]::FromImage($out)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
            $dest = New-Object System.Drawing.Rectangle 0, 0, $outW, $outH
            $srcRect = New-Object System.Drawing.Rectangle $cx, $cy, $cw, $ch
            $g.DrawImage($src, $dest, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
            $g.Dispose()
            $path = "$CropPrefix-$($pair[0]).png"
            $out.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
            $out.Dispose()
            $result['cropPaths'] = @($result['cropPaths']) + (Resolve-Path -LiteralPath $path).Path
        } finally {
            $src.Dispose()
        }
    }
}

$beforeImg.Bitmap.Dispose()
$afterImg.Bitmap.Dispose()
Write-Output ($result | ConvertTo-Json -Depth 6 -Compress)
