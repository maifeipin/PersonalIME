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
    $cTop = [System.Drawing.Color]::FromArgb(255, 10, 133, 234)
    $cBottom = [System.Drawing.Color]::FromArgb(255, 0, 64, 128)
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
    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(46, 255, 255, 255), [Math]::Max(1.0, 2.0 * $scale))
    $g.DrawPath($borderPen, $path)

    # Helper function to compute transformed coordinates
    # transform: matrix(sx 0 0 sy tx ty)
    $calcX = { param($x, $sx, $tx) [float](($x * $sx + $tx) * $scale) }
    $calcY = { param($y, $sy, $ty) [float](($y * $sy + $ty) * $scale) }

    # 1. Background echo "人" (Distant layer: scaled down to 0.82, shifted up-right, softer depth-of-field effect)
    $bsx = 0.82
    $bsy = 0.82
    $btx = 58.0
    $bty = -28.0

    # Multi-pass soft glow/bokeh simulation
    if ($Size -ge 48) {
        $haloStrokeW = [float](26.0 * $scale)
        $haloPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(40, 96, 213, 255), $haloStrokeW)
        $haloPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $haloPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $haloPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

        $g.DrawBezier($haloPen,
            (& $calcX 128 $bsx $btx), (& $calcY 56 $bsy $bty),
            (& $calcX 122 $bsx $btx), (& $calcY 108 $bsy $bty),
            (& $calcX 96  $bsx $btx), (& $calcY 156 $bsy $bty),
            (& $calcX 54  $bsx $btx), (& $calcY 196 $bsy $bty))
        $g.DrawBezier($haloPen,
            (& $calcX 116 $bsx $btx), (& $calcY 108 $bsy $bty),
            (& $calcX 138 $bsx $btx), (& $calcY 142 $bsy $bty),
            (& $calcX 166 $bsx $btx), (& $calcY 174 $bsy $bty),
            (& $calcX 202 $bsx $btx), (& $calcY 196 $bsy $bty))
        $haloPen.Dispose()
    }

    $backStrokeW = [Math]::Max(1.6, [float](21.0 * $scale))
    $backPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(145, 96, 213, 255), $backStrokeW)
    $backPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $backPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $backPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $g.DrawBezier($backPen,
        (& $calcX 128 $bsx $btx), (& $calcY 56 $bsy $bty),
        (& $calcX 122 $bsx $btx), (& $calcY 108 $bsy $bty),
        (& $calcX 96  $bsx $btx), (& $calcY 156 $bsy $bty),
        (& $calcX 54  $bsx $btx), (& $calcY 196 $bsy $bty))
    $g.DrawBezier($backPen,
        (& $calcX 116 $bsx $btx), (& $calcY 108 $bsy $bty),
        (& $calcX 138 $bsx $btx), (& $calcY 142 $bsy $bty),
        (& $calcX 166 $bsx $btx), (& $calcY 174 $bsy $bty),
        (& $calcX 202 $bsx $btx), (& $calcY 196 $bsy $bty))
    $backPen.Dispose()

    # 2. Foreground 3D Drop Shadow (simulating elevation above background "人")
    $fsx = 1.04
    $fsy = 1.04
    $ftx = -20.0
    $fty = 8.0

    if ($Size -ge 32) {
        $shadowStrokeW = [float](27.0 * $scale)
        $shadowPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(110, 0, 24, 51), $shadowStrokeW)
        $shadowPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $shadowPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $shadowPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

        $sdx = -2.5
        $sdy = 4.5
        $g.DrawBezier($shadowPen,
            (& $calcX 128 $fsx ($ftx + $sdx)), (& $calcY 56 $fsy ($fty + $sdy)),
            (& $calcX 122 $fsx ($ftx + $sdx)), (& $calcY 108 $fsy ($fty + $sdy)),
            (& $calcX 96  $fsx ($ftx + $sdx)), (& $calcY 156 $fsy ($fty + $sdy)),
            (& $calcX 54  $fsx ($ftx + $sdx)), (& $calcY 196 $fsy ($fty + $sdy)))
        $g.DrawBezier($shadowPen,
            (& $calcX 116 $fsx ($ftx + $sdx)), (& $calcY 108 $fsy ($fty + $sdy)),
            (& $calcX 138 $fsx ($ftx + $sdx)), (& $calcY 142 $fsy ($fty + $sdy)),
            (& $calcX 166 $fsx ($ftx + $sdx)), (& $calcY 174 $fsy ($fty + $sdy)),
            (& $calcX 202 $fsx ($ftx + $sdx)), (& $calcY 196 $fsy ($fty + $sdy)))
        $shadowPen.Dispose()
    }

    # 3. Foreground bold primary "人" (crisp pure white)
    $frontStrokeW = [Math]::Max(2.2, [float](27.0 * $scale))
    $frontPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $frontStrokeW)
    $frontPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $frontPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $frontPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

    $g.DrawBezier($frontPen,
        (& $calcX 128 $fsx $ftx), (& $calcY 56 $fsy $fty),
        (& $calcX 122 $fsx $ftx), (& $calcY 108 $fsy $fty),
        (& $calcX 96  $fsx $ftx), (& $calcY 156 $fsy $fty),
        (& $calcX 54  $fsx $ftx), (& $calcY 196 $fsy $fty))
    $g.DrawBezier($frontPen,
        (& $calcX 116 $fsx $ftx), (& $calcY 108 $fsy $fty),
        (& $calcX 138 $fsx $ftx), (& $calcY 142 $fsy $fty),
        (& $calcX 166 $fsx $ftx), (& $calcY 174 $fsy $fty),
        (& $calcX 202 $fsx $ftx), (& $calcY 196 $fsy $fty))
    $frontPen.Dispose()

    $brush.Dispose()
    $borderPen.Dispose()
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

Write-Host "Icons regenerated successfully with 3D depth and bokeh effect." -ForegroundColor Green
