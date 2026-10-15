<#
.SYNOPSIS  Samples a Windows process: private/working-set memory, CPU %, threads, handles.
.EXAMPLE   powershell -File bench/measure_process.ps1 -Exe .\prometheus99.exe -Seconds 20 -Out bench/results/legacy.csv
Host measurement only. Raw samples go to the CSV; a summary is printed.
#>
param([string]$Exe, [int]$Seconds = 20, [string]$Out = "bench/results/process.csv", [int]$IntervalMs = 500)
$full = (Resolve-Path $Exe).Path
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $full -PassThru -WindowStyle Minimized
$startup = $null
while ($sw.ElapsedMilliseconds -lt 5000) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { $startup = $sw.ElapsedMilliseconds; break }
    Start-Sleep -Milliseconds 10
}
$ncpu = [Environment]::ProcessorCount
$rows = @()
$prevCpu = $p.TotalProcessorTime.TotalMilliseconds
$prevT = $sw.ElapsedMilliseconds
$end = $sw.ElapsedMilliseconds + $Seconds * 1000
while ($sw.ElapsedMilliseconds -lt $end -and -not $p.HasExited) {
    Start-Sleep -Milliseconds $IntervalMs
    $p.Refresh()
    $t = $sw.ElapsedMilliseconds
    $cpu = $p.TotalProcessorTime.TotalMilliseconds
    $pct = 100.0 * ($cpu - $prevCpu) / (($t - $prevT) * $ncpu)
    $prevCpu = $cpu; $prevT = $t
    $rows += [pscustomobject]@{
        t_ms = $t; private_kb = [math]::Round($p.PrivateMemorySize64/1KB); working_set_kb = [math]::Round($p.WorkingSet64/1KB)
        peak_ws_kb = [math]::Round($p.PeakWorkingSet64/1KB); cpu_pct_of_machine = [math]::Round($pct,3)
        threads = $p.Threads.Count; handles = $p.HandleCount
    }
}
if (-not $p.HasExited) { $null = $p.CloseMainWindow(); Start-Sleep -Milliseconds 500; if (-not $p.HasExited) { $p.Kill() } }
$rows | Export-Csv -NoTypeInformation -Path $Out
$m = $rows | Measure-Object cpu_pct_of_machine -Average -Maximum
Write-Output ("exe={0} samples={1} interval={2}ms logical_cpus={3}" -f $Exe, $rows.Count, $IntervalMs, $ncpu)
Write-Output ("startup_to_window_ms={0}" -f $startup)
Write-Output ("private_kb: min={0} max={1}" -f ($rows.private_kb | Measure-Object -Minimum).Minimum, ($rows.private_kb | Measure-Object -Maximum).Maximum)
Write-Output ("working_set_kb: min={0} max={1} peak={2}" -f ($rows.working_set_kb | Measure-Object -Minimum).Minimum, ($rows.working_set_kb | Measure-Object -Maximum).Maximum, ($rows.peak_ws_kb | Measure-Object -Maximum).Maximum)
Write-Output ("cpu_pct_of_machine: avg={0} max={1}" -f [math]::Round($m.Average,3), $m.Maximum)
Write-Output ("threads: {0}  handles: min={1} max={2}" -f $rows[-1].threads, ($rows.handles | Measure-Object -Minimum).Minimum, ($rows.handles | Measure-Object -Maximum).Maximum)
