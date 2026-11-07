Add-Type -AssemblyName System.Drawing

function New-RenRenIconBitmap {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $scale = $Size / 256.0

    # Gradient background
    $rect = New-Object System.Drawing.RectangleF(0, 0, $Size, $Size)
    $cTop = [System.Drawing.Color]::FromArgb(255, 0, 120, 212)
    $cBottom = [System.Drawing.Color]::FromArgb(255, 0, 78, 140)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $cTop, $cBottom, 45.0)

    # Rounded rectangle path (squircle)
    $corner = [Math]::Max(2.0, [float](54.0 * $scale))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float]($corner * 2.0)
    $pad = [float](1.0 * $scale)
    $dim = [float]($Size - 2.0 * $pad)
    $r = New-Object System.Drawing.RectangleF($pad, $pad, $dim, $dim)

    $path.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $path.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $path.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $g.FillPath($brush, $path)

    # Subtle inner border
    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(50, 255, 255, 255), [Math]::Max(1.0, 2.0 * $scale))
    $g.DrawPath($borderPen, $path)

    # Layered "人人" Design:
    # 1. Background echo "人" (offset top-right, translucent cyan)
    $backStrokeW = [Math]::Max(1.8, [float](24.0 * $scale))
    $backPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(150, 124, 232, 255), $backStrokeW)
    $backPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $backPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $backPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $bx = 26.0
    $by = -16.0
    $g.DrawBezier($backPen,
        [float]((128 + $bx) * $scale), [float]((56 + $by) * $scale),
        [float]((122 + $bx) * $scale), [float]((108 + $by) * $scale),
        [float]((96 + $bx) * $scale),  [float]((156 + $by) * $scale),
        [float]((54 + $bx) * $scale),  [float]((196 + $by) * $scale))
    $g.DrawBezier($backPen,
        [float]((116 + $bx) * $scale), [float]((108 + $by) * $scale),
        [float]((138 + $bx) * $scale), [float]((142 + $by) * $scale),
        [float]((166 + $bx) * $scale), [float]((174 + $by) * $scale),
        [float]((202 + $bx) * $scale), [float]((196 + $by) * $scale))

    # 2. Foreground bold primary "人" (crisp white)
    $frontStrokeW = [Math]::Max(2.2, [float](26.0 * $scale))
    $frontPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $frontStrokeW)
    $frontPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $frontPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $frontPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $fx = -10.0
    $fy = 8.0
    $g.DrawBezier($frontPen,
        [float]((128 + $fx) * $scale), [float]((56 + $fy) * $scale),
        [float]((122 + $fx) * $scale), [float]((108 + $fy) * $scale),
        [float]((96 + $fx) * $scale),  [float]((156 + $fy) * $scale),
        [float]((54 + $fx) * $scale),  [float]((196 + $fy) * $scale))
    $g.DrawBezier($frontPen,
        [float]((116 + $fx) * $scale), [float]((108 + $fy) * $scale),
        [float]((138 + $fx) * $scale), [float]((142 + $fy) * $scale),
        [float]((166 + $fx) * $scale), [float]((174 + $fy) * $scale),
        [float]((202 + $fx) * $scale), [float]((196 + $fy) * $scale))

    $brush.Dispose()
    $borderPen.Dispose()
    $backPen.Dispose()
    $frontPen.Dispose()
    $path.Dispose()
    $g.Dispose()

    return $bmp
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$bmps = @{}
foreach ($s in $sizes) {
    $bmps[$s] = New-RenRenIconBitmap -Size $s
}

$bmps[256].Save("assets/app.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmps[256].Save("src/manager/PersonalIME.Manager/Assets/app.png", [System.Drawing.Imaging.ImageFormat]::Png)

function Write-IcoFile {
    param([string]$OutPath, [hashtable]$Bitmaps, [int[]]$SizesList)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    $bw.Write([uint16]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]$SizesList.Count)

    $pngBuffers = @()
    foreach ($s in $SizesList) {
        $pStream = New-Object System.IO.MemoryStream
        $Bitmaps[$s].Save($pStream, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngBuffers += ,($pStream.ToArray())
        $pStream.Dispose()
    }

    $offset = 6 + (16 * $SizesList.Count)
    for ($i = 0; $i -lt $SizesList.Count; $i++) {
        $s = $SizesList[$i]
        $buf = $pngBuffers[$i]
        $wByte = if ($s -ge 256) { [byte]0 } else { [byte]$s }
        $hByte = if ($s -ge 256) { [byte]0 } else { [byte]$s }

        $bw.Write($wByte)
        $bw.Write($hByte)
        $bw.Write([byte]0)
        $bw.Write([byte]0)
        $bw.Write([uint16]1)
        $bw.Write([uint16]32)
        $bw.Write([uint32]$buf.Length)
        $bw.Write([uint32]$offset)

        $offset += $buf.Length
    }

    for ($i = 0; $i -lt $SizesList.Count; $i++) {
        $bw.Write($pngBuffers[$i])
    }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($OutPath, $ms.ToArray())
    $bw.Dispose()
    $ms.Dispose()
}

Write-IcoFile -OutPath "assets/app.ico" -Bitmaps $bmps -SizesList $sizes
Write-IcoFile -OutPath "src/manager/PersonalIME.Manager/Assets/app.ico" -Bitmaps $bmps -SizesList $sizes
Write-IcoFile -OutPath "src/tsf_client/app.ico" -Bitmaps $bmps -SizesList $sizes

foreach ($s in $sizes) {
    $bmps[$s].Dispose()
}

Write-Host "Icons generated successfully." -ForegroundColor Green
