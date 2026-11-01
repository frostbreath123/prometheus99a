# One picture with the same three moments (high alarm, setpoint written, PLC disconnected) from each target.
#   powershell -ExecutionPolicy Bypass -File demo\make_overview_sheet.ps1
Add-Type -AssemblyName System.Drawing
$root = Join-Path (Split-Path $PSScriptRoot) "evidence"
$rows = @(
    @{ label = "Windows demo";            dir = "windows";     files = @("02_alarm_high_unacknowledged", "05_setpoint_written", "08_plc_disconnected") },
    @{ label = "Linux (SDL window)";      dir = "linux";       files = @("02_alarm_high_unacknowledged", "05_setpoint_written", "08_plc_disconnected") },
    @{ label = "Raspberry Pi (emulated)"; dir = "pi_emulated"; files = @("02_state_a", "05_setpoint_written", "08_plc_disconnected") }
)
$w = 640; $h = 384; $pad = 28
$bmp = New-Object System.Drawing.Bitmap ($w * 3), (($h + $pad) * 3)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(24, 24, 28))
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object System.Drawing.Font "Segoe UI", 12, ([System.Drawing.FontStyle]::Bold)
for ($r = 0; $r -lt 3; $r++) {
    $y = $r * ($h + $pad)
    $g.DrawString($rows[$r].label, $font, [System.Drawing.Brushes]::White, 6, $y + 4)
    for ($c = 0; $c -lt 3; $c++) {
        $img = [System.Drawing.Image]::FromFile((Join-Path (Join-Path $root $rows[$r].dir) ($rows[$r].files[$c] + ".png")))
        $g.DrawImage($img, $c * $w, $y + $pad, $w, $h)
        $img.Dispose()
    }
}
$out = Join-Path $root "overview_all_targets.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
$out
