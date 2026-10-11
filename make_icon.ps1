# 원본 PNG -> tomato-sauce.ico (16~256px, PNG 압축 엔트리)
param([string]$Source = "$PSScriptRoot\icon\tomato-sauce.png")
Add-Type -AssemblyName System.Drawing
$dir = $PSScriptRoot
$src = [System.Drawing.Bitmap]::FromFile($Source)

# 투명하지 않은 영역만 잘라내서 정사각형으로
$minX = $src.Width; $minY = $src.Height; $maxX = 0; $maxY = 0
for ($y = 0; $y -lt $src.Height; $y += 2) {
    for ($x = 0; $x -lt $src.Width; $x += 2) {
        if ($src.GetPixel($x, $y).A -gt 10) {
            if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$side = [Math]::Max($maxX - $minX, $maxY - $minY) + 4
$cx = ($minX + $maxX) / 2; $cy = ($minY + $maxY) / 2
$crop = New-Object System.Drawing.RectangleF(($cx - $side / 2), ($cy - $side / 2), $side, $side)

$sizes = 256, 128, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.SmoothingMode = 'HighQuality'
    $g.DrawImage($src, (New-Object System.Drawing.RectangleF(0, 0, $s, $s)), $crop, 'Pixel')
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $bmp.Save("$dir\icon\icon-256.png") }
    $bmp.Dispose()
    ,$ms.ToArray()
}
$src.Dispose()

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $pngs[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$d.Length); $w.Write([UInt32]$offset)
    $offset += $d.Length
}
foreach ($d in $pngs) { $w.Write($d) }
[System.IO.File]::WriteAllBytes("$dir\tomato-sauce.ico", $out.ToArray())
"tomato-sauce.ico 생성 완료"

