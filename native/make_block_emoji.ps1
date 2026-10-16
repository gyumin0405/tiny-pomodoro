# emoji/block.png (128px) 생성: 🚫 금지 표시를 다른 이모지처럼 입체감 있는 그라데이션으로 그림
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot
$N = 128
$bmp = New-Object System.Drawing.Bitmap $N, $N, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.PixelOffsetMode = 'HighQuality'

$c = $N / 2; $ro = 56; $ri = 40; $bar = 17
$outer = New-Object System.Drawing.RectangleF(($c - $ro), ($c - $ro), (2 * $ro), (2 * $ro))
$inner = New-Object System.Drawing.RectangleF(($c - $ri), ($c - $ri), (2 * $ri), (2 * $ri))

# 고리 + 사선 (사선은 안쪽 원 안에서만)
$ring = New-Object System.Drawing.Drawing2D.GraphicsPath
$ring.FillMode = 'Alternate'
$ring.AddEllipse($outer); $ring.AddEllipse($inner)
$slash = New-Object System.Drawing.Drawing2D.GraphicsPath
$slash.AddRectangle((New-Object System.Drawing.RectangleF(($c - $ri - 2), ($c - $bar / 2), (2 * $ri + 4), $bar)))
$m = New-Object System.Drawing.Drawing2D.Matrix
$m.RotateAt(45, (New-Object System.Drawing.PointF($c, $c)))
$slash.Transform($m)
$region = New-Object System.Drawing.Region $ring
$region.Union($slash)

# 위-왼쪽이 밝고 아래-오른쪽이 진한 붉은 그라데이션 (fire/coffee 이모지 느낌)
$lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.PointF(16, 10)), (New-Object System.Drawing.PointF(112, 118)),
    [System.Drawing.Color]::FromArgb(255, 255, 122, 108), [System.Drawing.Color]::FromArgb(255, 196, 30, 52))
$g.FillRegion($lg, $region)

# 은은한 하이라이트 (위쪽만)
$g.SetClip($region, [System.Drawing.Drawing2D.CombineMode]::Replace)
$hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.PointF(0, 6)), (New-Object System.Drawing.PointF(0, 70)),
    [System.Drawing.Color]::FromArgb(70, 255, 255, 255), [System.Drawing.Color]::FromArgb(0, 255, 255, 255))
$g.FillEllipse($hl, ($c - $ro + 8), ($c - $ro + 2), (2 * $ro - 16), $ro)
$g.ResetClip()
$g.Dispose()
$bmp.Save("$root\emoji\block.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"emoji/block.png generated"
