# Runs the scripted tour of the Windows demo, converts the pictures it saved to PNG, and records memory, CPU and size.
# Evidence and test tool. The demo itself writes the pictures (hmi_win_demo.c, --tour), so they work on a locked desktop.
#   powershell -ExecutionPolicy Bypass -File demo\win_capture.ps1 -Exe build-windows\hmi_win_demo.exe -Out evidence\windows
param(
    [string]$Exe = "build-windows\hmi_win_demo.exe",
    [string]$Out = "evidence\windows"
)
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outPath = (Resolve-Path $Out).Path
Get-ChildItem $outPath -Include *.bmp, *.png, tour.log -ErrorAction SilentlyContinue | Remove-Item -Force
$exePath = (Resolve-Path $Exe).Path
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exePath -ArgumentList "--tour", $outPath -WorkingDirectory (Split-Path $exePath) -PassThru
$rows = @()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt 60) {
    Start-Sleep -Milliseconds 1000
    try {
        $p.Refresh()
        $c = Get-Counter "\Process($($p.ProcessName))\Working Set - Private" -ErrorAction Stop
        $rows += [pscustomobject]@{
            t = [math]::Round($sw.Elapsed.TotalSeconds, 1)
            private_ws_kb = [int]($c.CounterSamples[0].CookedValue / 1KB)
            private_bytes_kb = [int]($p.PrivateMemorySize64 / 1KB)
            working_set_kb = [int]($p.WorkingSet64 / 1KB)
        }
    } catch { }
}
$exited = $p.HasExited
$cpu = $null
if ($exited) { $cpu = $p.TotalProcessorTime.TotalSeconds } else { $cpu = $p.TotalProcessorTime.TotalSeconds; Stop-Process -Id $p.Id }
$wall = $sw.Elapsed.TotalSeconds
$n = 0
foreach ($b in Get-ChildItem $outPath -Filter *.bmp) {
    $img = [System.Drawing.Image]::FromFile($b.FullName)
    $img.Save((Join-Path $outPath ($b.BaseName + ".png")), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $b.FullName
    $n++
}
$sum = @()
$sum += "exe_bytes=" + (Get-Item $exePath).Length
$sum += "tour_finished_by_itself=" + $exited
$sum += "wall_seconds=" + [math]::Round($wall, 1)
$sum += "cpu_seconds=" + [math]::Round($cpu, 2) + "  avg_cpu_percent_of_one_core=" + [math]::Round(100 * $cpu / $wall, 2)
$sum += "pictures=" + $n
if ($rows.Count -gt 0) {
    $sum += "private_ws_kb  min/max=" + (($rows | Measure-Object private_ws_kb -Minimum -Maximum | ForEach-Object { "$($_.Minimum)/$($_.Maximum)" }))
    $sum += "private_bytes_kb  max=" + ($rows | Measure-Object private_bytes_kb -Maximum).Maximum
    $sum += "working_set_kb  max=" + ($rows | Measure-Object working_set_kb -Maximum).Maximum
    $sum += "samples=" + $rows.Count
}
$sum | Set-Content (Join-Path $outPath "measure.txt")
$sum
