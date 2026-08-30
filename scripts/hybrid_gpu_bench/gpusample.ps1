param([int]$Seconds = 20, [string]$Label = "sample", [hashtable]$Names = $null)
# Per-adapter + per-process GPU busy via \GPU Engine(*)\Running Time deltas (NOT Utilization%).
# Instance: pid_<pid>_luid_0x00000000_0x000XXXXX_phys_0_eng_N_engtype_<type>
#
# LUIDs ARE NOT STABLE - they are reassigned across reboots and driver
# restarts, so the map below is a convenience for one box at one time, not a
# fact. Pass -Names @{ '0X000XXXXX' = 'iGPU'; ... } to label your own; any LUID
# not in the map still reports, under its raw LUID, and the run warns so a
# stale map cannot quietly relabel one adapter as the other. Read the scanout
# adapter's LUID off `displayxr-cli info` (or a runtime log's "scanout: display
# path ... is driven by adapter ..." line) when building a map.
$ErrorActionPreference = 'Stop'
$names = if ($Names -ne $null) { $Names } else { @{ '0X00024BBF' = 'iGPU'; '0X00024F0B' = 'dGPU'; '0X00024E73' = 'gpu24e73' } }
$unmapped = @{}
$a = (Get-Counter '\GPU Engine(*)\Running Time' -MaxSamples 1).CounterSamples
$t0 = [System.Diagnostics.Stopwatch]::GetTimestamp()
Start-Sleep -Seconds $Seconds
$b = (Get-Counter '\GPU Engine(*)\Running Time' -MaxSamples 1).CounterSamples
$t1 = [System.Diagnostics.Stopwatch]::GetTimestamp()
$elapsed = ($t1 - $t0) / [System.Diagnostics.Stopwatch]::Frequency
$m = @{}
foreach ($c in $a) { $m[$c.InstanceName] = $c.CookedValue }
$rows = @()
foreach ($c in $b) {
    if (-not $m.ContainsKey($c.InstanceName)) { continue }
    $d = $c.CookedValue - $m[$c.InstanceName]
    if ($d -le 0) { continue }
    if ($c.InstanceName -match 'pid_(\d+)_luid_0x[0-9A-Fa-f]+_(0x[0-9A-Fa-f]+)_phys_\d+_eng_\d+_engtype_(\w+)') {
        $procId = [int]$Matches[1]
        $luid = $Matches[2].ToUpper().Replace('0X','0X')
        $nm = if ($names.ContainsKey($luid)) { $names[$luid] } else { $unmapped[$luid] = $true; $Matches[2] }
        $pn = try { (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { "pid$procId" }
        $rows += [pscustomobject]@{ Proc=$pn; Gpu=$nm; Eng=$Matches[3]; Ms=$d/10000.0/$elapsed }
    }
}
"== gpusample [$Label] elapsed=$([math]::Round($elapsed,2))s (busy ms per wall second) =="
if ($unmapped.Count -gt 0) {
    "!! LUID(s) not in the name map: $($unmapped.Keys -join ', ') - reported under their raw LUID."
    '!! LUIDs change across reboots; pass -Names to label this box (see the header).'
}
"-- per process/adapter/engine (>=1 ms/s) --"
$rows | Group-Object Proc, Gpu, Eng | ForEach-Object {
    $s = ($_.Group | Measure-Object Ms -Sum).Sum
    [pscustomobject]@{ Key=$_.Name; Ms=$s }
} | Where-Object { $_.Ms -ge 1 } | Sort-Object Ms -Descending | ForEach-Object {
    "{0,-52} {1,8:F1} ms/s" -f $_.Key, $_.Ms
}
"-- adapter totals (all processes) --"
$rows | Group-Object Gpu | ForEach-Object {
    $s = ($_.Group | Measure-Object Ms -Sum).Sum
    "{0,-10} TOTAL {1,8:F1} ms/s ({2:F2}%)" -f $_.Name, $s, ($s/10.0)
}
"-- adapter totals EXCLUDING Cursor --"
$rows | Where-Object { $_.Proc -ne 'Cursor' } | Group-Object Gpu | ForEach-Object {
    $s = ($_.Group | Measure-Object Ms -Sum).Sum
    "{0,-10} TOTAL {1,8:F1} ms/s ({2:F2}%)" -f $_.Name, $s, ($s/10.0)
}
