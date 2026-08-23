# run_crossover.ps1 - #1154 adapter-placement crossover measurement.
#
# Not the app ladder: this drives the SERVICE path (service + IPC client +
# a GPU load generator), restarts the service per arm because the split is a
# service-startup env, and asserts per-arm from the SERVICE LOG rather than
# inferring from the env it set. Sampling, bucket accounting and medians are
# reused from lib_sample.ps1.
#
# Arms (2 of 3; the all-on-scanout arm waits on #1153):
#   render : service default            - implicit cross-adapter present
#   split  : DXR_WEAVE_ON_SCANOUT=1     - explicit bridge, weave on scanout
# Render weight is swept with gpu_loadgen duty so exactly ONE parameter varies.
#
# PowerShell 5.1, ASCII only. Run NON-ELEVATED work via explorer.exe: an
# elevated process ignores XR_RUNTIME_JSON and silently loads the INSTALLED
# runtime, which then fails the client/service tag gate (or worse, passes it
# against a different tree).
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot 'crossover.json'),
    [string]$OutRoot = (Join-Path $PSScriptRoot 'results'),
    [switch]$Sanity,          # duty-0 only, both arms, assertions only, nothing kept
    [int]$RepsOverride = 0
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

$cfg = Get-Content $ConfigPath -Raw | ConvertFrom-Json
$reps = if ($RepsOverride -gt 0) { $RepsOverride } elseif ($Sanity) { 1 } else { $cfg.reps }
$duties = if ($Sanity) { @(0) } else { @($cfg.duties) }
$windowSec = if ($Sanity) { 8 } else { $cfg.windowSeconds }
$warmupSec = if ($Sanity) { 4 } else { $cfg.warmupSeconds }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $OutRoot ('crossover-' + $env:COMPUTERNAME + '-' + $stamp)
New-Item -ItemType Directory -Force $outDir | Out-Null
$csvPath = Join-Path $outDir 'crossover.csv'
$runLog = Join-Path $outDir 'run.log'
function Log { param([string]$m)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
    Write-Host $line; Add-Content -Path $runLog -Value $line
}

# Config paths may be repo-relative (portable) or absolute (box-specific).
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
function Abs { param([string]$p)
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $repoRoot $p)
}
$tree = Abs $cfg.treeRoot
$svcExe = Join-Path $tree 'bin\displayxr-service.exe'
$cliExe = Abs $cfg.clientExe
$loadExe = Abs $cfg.loadExe
$runtimeJson = Abs $cfg.runtimeJson
$runtimePath = Join-Path $tree 'bin'
foreach ($p in @($svcExe, $cliExe, $loadExe, $runtimeJson)) {
    if (-not (Test-Path $p)) { throw ('missing: ' + $p) }
}
$svcBase = [IO.Path]::GetFileNameWithoutExtension($svcExe)
$cliBase = [IO.Path]::GetFileNameWithoutExtension($cliExe)
$logDir = Join-Path $env:LOCALAPPDATA 'DisplayXR'

# ---------------------------------------------------------------- helpers ---
function Stop-Named { param([string[]]$names)
    foreach ($n in $names) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}

function New-LauncherBat {
    # env must be process-level: the runtime DLL carries its own static-CRT
    # environment block and will not see a value set after it loads.
    param([string]$tag, [hashtable]$env, [string]$exe, [string]$args)
    $bat = Join-Path $env:TEMP ('xover_' + $tag + '.bat')
    $lines = @('@echo off')
    foreach ($k in ($env.Keys | Sort-Object)) { $lines += ('set "{0}={1}"' -f $k, $env[$k]) }
    $lines += ('"{0}" {1} > "%TEMP%\xover_{2}.out" 2>&1' -f $exe, $args, $tag)
    Set-Content -Path $bat -Value ($lines -join "`r`n") -Encoding Ascii
    return $bat
}

function Start-NonElevated { param([string]$bat)
    # explorer.exe strips elevation; it returns exit code 1 ON SUCCESS, so
    # never chain on its exit status.
    Start-Process explorer.exe -ArgumentList $bat | Out-Null
}

function Wait-ForProcess { param([string]$name, [datetime]$after, [int]$timeoutSec)
    $end = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $end) {
        $p = Get-Process -Name $name -ErrorAction SilentlyContinue |
             Where-Object { $_.StartTime -gt $after.AddSeconds(-2) } | Select-Object -First 1
        if ($p -ne $null) { return $p }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Get-LogForPid {
    # The service writes TWO logs per pid; match by PID, never by newest-file.
    param([string]$base, [int]$procId)
    $f = Get-ChildItem $logDir -Filter ('DisplayXR_' + $base + '.exe.' + $procId + '_*.log') -ErrorAction SilentlyContinue |
         Sort-Object Length -Descending | Select-Object -First 1
    if ($f -eq $null) { return $null }
    return $f.FullName
}

function Wait-ForLine { param([string]$path, [string]$pattern, [int]$timeoutSec)
    $end = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $end) {
        if ($path -ne $null -and (Test-Path $path)) {
            if ((Select-String -Path $path -Pattern $pattern -SimpleMatch -Quiet -ErrorAction SilentlyContinue)) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Has-Line { param([string]$path, [string]$pattern)
    if ($path -eq $null -or -not (Test-Path $path)) { return $false }
    return [bool](Select-String -Path $path -Pattern $pattern -SimpleMatch -Quiet -ErrorAction SilentlyContinue)
}

function LastMatch { param([string]$path, [string]$regex)
    if ($path -eq $null -or -not (Test-Path $path)) { return $null }
    $m = Select-String -Path $path -Pattern $regex -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($m -eq $null) { return $null }
    return $m.Matches[0]
}

# ---------------------------------------------------------------- one cell --
function Invoke-Cell {
    param([string]$armName, [hashtable]$svcEnv, [int]$duty, [int]$rep)
    $flags = @(); $abort = $null
    $tag = ($armName + '-d' + $duty + '-r' + $rep)
    Stop-Named @($cliBase, 'gpu_loadgen', $svcBase)

    # --- service, non-elevated, arm env -------------------------------------
    $t0 = Get-Date
    $wlPrefix = Join-Path $outDir ('wl_' + $tag)
    $env0 = @{ 'DXR_SPLIT_COVER_DIAG' = '1'; 'DXR_FRAME_WITNESS' = '5'
               'DXR_WEAVE_LATENCY_CSV' = $wlPrefix
               'PATH' = ($runtimePath + ';' + $env:PATH) }
    foreach ($k in $svcEnv.Keys) { $env0[$k] = $svcEnv[$k] }
    Start-NonElevated (New-LauncherBat ('svc_' + $tag) $env0 $svcExe '')
    $svcProc = Wait-ForProcess $svcBase $t0 40
    if ($svcProc -eq $null) { return @{ ok = $false; abort = 'SERVICE_NO_START' } }
    $svcLog = $null
    $end = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $end -and $svcLog -eq $null) { $svcLog = Get-LogForPid $svcBase $svcProc.Id; Start-Sleep -Milliseconds 500 }
    if ($svcLog -eq $null) { Stop-Named @($svcBase); return @{ ok = $false; abort = 'SERVICE_NO_LOG' } }

    # --- client, non-elevated, forced IPC, same tree -------------------------
    $t1 = Get-Date
    $cenv = @{ 'XRT_FORCE_MODE' = 'ipc'; 'XR_RUNTIME_JSON' = $runtimeJson
               'DXR_FRAME_WITNESS' = '5'; 'PATH' = ($runtimePath + ';' + $env:PATH) }
    Start-NonElevated (New-LauncherBat ('cli_' + $tag) $cenv $cliExe '')
    $cliProc = Wait-ForProcess $cliBase $t1 40
    if ($cliProc -eq $null) { Stop-Named @($svcBase); return @{ ok = $false; abort = 'CLIENT_NO_START' } }
    $cliLog = $null
    $end = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $end -and $cliLog -eq $null) { $cliLog = Get-LogForPid $cliBase $cliProc.Id; Start-Sleep -Milliseconds 500 }

    # --- provenance + assertions (from the LOG, never from the env we set) ---
    $tagLine = LastMatch $svcLog "runtime .* '([^']+)' loaded from: (.+?) \("
    $svcTag = if ($tagLine) { $tagLine.Groups[1].Value } else { '?' }
    $svcFrom = if ($tagLine) { $tagLine.Groups[2].Value } else { '?' }
    $cliFromM = LastMatch $cliLog "loaded from: (.+?) \("
    $cliFrom = if ($cliFromM) { $cliFromM.Groups[1].Value } else { '?' }
    foreach ($pair in @(@($svcFrom, 'SVC'), @($cliFrom, 'CLI'))) {
        if ($pair[0] -notlike ($tree + '*')) { $flags += ($pair[1] + '_WRONG_TREE') ; $abort = 'WRONG_TREE' }
    }
    if (Has-Line $cliLog 'XR_ERROR_RUNTIME_VERSION_SKEW') { $abort = 'TAG_SKEW' }

    # 1. presenter kind - CLIENT_TEXTURE is structurally ineligible
    (Wait-ForLine $svcLog '[pipeline] presenter=' 30) | Out-Null
    $presM = LastMatch $svcLog '\[pipeline\] presenter=(\w+)'
    $presenter = if ($presM) { $presM.Groups[1].Value } else { 'UNKNOWN' }
    if ($presenter -eq 'CLIENT_TEXTURE') { $abort = 'PRESENTER_INELIGIBLE' }
    if ($presenter -eq 'UNKNOWN') { $flags += 'NO_PRESENTER_LINE' }

    # 2/3. split engaged exactly where it should be
    $splitActive = Has-Line $svcLog '#918 output-device split ACTIVE'
    $splitNoop = Has-Line $svcLog 'IS the service''s adapter'
    if ($armName -eq 'split') {
        if ($splitNoop) { $abort = 'SPLIT_NOOP_SAME_ADAPTER' }
        elseif (-not $splitActive) { $abort = 'SPLIT_DID_NOT_ENGAGE' }
    } else {
        if ($splitActive) { $abort = 'SPLIT_ACTIVE_IN_CONTROL' }
    }
    if ($abort -ne $null) {
        Stop-Named @($cliBase, $svcBase)
        return @{ ok = $false; abort = $abort; flags = $flags; svcTag = $svcTag; presenter = $presenter }
    }

    # --- load generator ------------------------------------------------------
    $loadDesc = ''
    if ($duty -gt 0) {
        $secs = $warmupSec + $windowSec + 15
        $largs = ('--adapter=' + $cfg.loadAdapter + ' --duty=' + $duty + ' --seconds=' + $secs + ' --res=' + $cfg.loadRes)
        Start-NonElevated (New-LauncherBat ('load_' + $tag) @{ 'PATH' = $env:PATH } $loadExe $largs)
        Start-Sleep -Seconds 3
        $lo = Join-Path $env:TEMP ('xover_load_' + $tag + '.out')
        if (Test-Path $lo) {
            $d = Select-String -Path $lo -Pattern 'adapter|Adapter' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($d) { $loadDesc = ($d.Line -replace ',', ';').Trim() }
        }
    }

    Start-Sleep -Seconds $warmupSec
    $witBefore = @(Get-WitnessLines $cliLog).Count
    $svcPids = @(Get-Process -Name $svcBase -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $cpu0 = Get-CpuSnapshot -procIds (@($cliProc.Id) + $svcPids)
    $g0 = Get-GpuSnapshot
    Start-Sleep -Seconds $windowSec
    $g1 = Get-GpuSnapshot
    $cpu1 = Get-CpuSnapshot -procIds (@($cliProc.Id) + $svcPids)
    if (-not $g0.ok -or -not $g1.ok) { $flags += 'GPU_COUNTER_FAIL' }

    # --- per-tick truth + correctness gate, read AFTER the window -----------
    $renderM = LastMatch $svcLog '\[RENDER\] split=(\d+).*?out_crop=(\d+).*?ing_leak=(\d+)'
    $rSplit = if ($renderM) { [int]$renderM.Groups[1].Value } else { -1 }
    $rCrop = if ($renderM) { [int]$renderM.Groups[2].Value } else { -1 }
    $rLeak = if ($renderM) { [int]$renderM.Groups[3].Value } else { -1 }
    $coverM = LastMatch $svcLog '\[COVER\].*?post_black=(\d+) in_black=(\d+)'
    $postBlack = if ($coverM) { [int]$coverM.Groups[1].Value } else { -1 }
    $inBlack = if ($coverM) { [int]$coverM.Groups[2].Value } else { -1 }
    # R: panel_r_ns is computed and fed to the DP but the PERIODIC [COVER] line
    # never prints it - only the black-burst START/END lines do, and those fire
    # exactly when the run is unhealthy. So the per-presenter CSV ledger
    # (DXR_WEAVE_LATENCY_CSV -> <prefix>.<site>.csv, site = workspace |
    # apphwnd.sN) is the real instrument here, not the backup.
    # R comes off the periodic [COVER] line as of #1157: r_p50/r_p95 are
    # PERCENTILES (how R is quoted everywhere in #918 - a mean here would be a
    # different metric), zero residuals are EXCLUDED rather than averaged in,
    # and r_n is the sample count over a 256-deep tail ring. r_n=0 with 0.00
    # percentiles therefore reads "nothing measured", never "instant".
    $rM = LastMatch $svcLog 'r_p50=([\d.]+) r_p95=([\d.]+) r_n=(\d+)'
    $rP50 = if ($rM) { [double]$rM.Groups[1].Value } else { -1 }
    $rP95 = if ($rM) { [double]$rM.Groups[2].Value } else { -1 }
    $rN = if ($rM) { [int]$rM.Groups[3].Value } else { 0 }
    if ($rN -le 0) { $flags += 'NO_R_SAMPLES' }
    # CSV stays armed for provenance + distribution: it names WHICH chain wove
    # (workspace vs apphwnd.sN), which the summary line does not.
    $wlFiles = @(Get-ChildItem ($wlPrefix + '.*.csv') -ErrorAction SilentlyContinue)
    $wlRows = 0; $wlSites = @()
    foreach ($wf in $wlFiles) {
        $c = @(Select-String -Path $wf.FullName -Pattern '^F,' -ErrorAction SilentlyContinue).Count
        $wlRows += $c
        if ($c -gt 0) { $wlSites += ($wf.BaseName -replace '^wl_[^.]*\.', '') }
    }
    $wlSite = ($wlSites -join '+')
    $trackM = LastMatch $svcLog '\[COVER\] split=\d+ tracking=(\d+)'
    $tracking = if ($trackM) { [int]$trackM.Groups[1].Value } else { -1 }

    $expected = if ($armName -eq 'split') { 1 } else { 0 }
    if ($rSplit -ne $expected) { $flags += ('RENDER_SPLIT_' + $rSplit) }
    if ($rLeak -gt 0) { $flags += ('ING_LEAK_' + $rLeak) }
    if ($armName -eq 'split' -and $rCrop -le 0) { $flags += 'OUT_CROP_ZERO' }
    if ($postBlack -ne 0 -or $inBlack -ne 0) { $flags += ('COVER_BLACK_' + $postBlack + '_' + $inBlack) }

    $wit = @(); $all = @(Get-WitnessLines $cliLog)
    if ($all.Count -gt $witBefore) { $wit = $all[$witBefore..($all.Count - 1)] }
    $wp = 0.0; $ww = 0.0
    if ($wit.Count -gt 0) {
        $wp = Get-TrueMedian @($wit | ForEach-Object { $_.PresentsPerS })
        $ww = Get-TrueMedian @($wit | ForEach-Object { $_.WeavesPerS })
    } else { $flags += 'NO_WITNESS' }

    Stop-Named @($cliBase, 'gpu_loadgen', $svcBase)
    $delta = Get-GpuDelta $g0 $g1
    if (-not $delta.ok) { return @{ ok = $false; abort = 'GPU_DELTA_FAIL'; flags = $flags } }
    $col = Resolve-GpuColumns -deltaRows $delta.rows -appPids @($cliProc.Id) -svcPids $svcPids -scanoutLuid $scanoutLuid
    $cpu = Get-CpuDeltaPct $cpu0 $cpu1

    return @{ ok = $true; flags = $flags; col = $col; cpu = $cpu; presents = $wp; weaves = $ww
              rP50 = $rP50; rP95 = $rP95; rN = $rN; wlSite = $wlSite; rSplit = $rSplit; rCrop = $rCrop; postBlack = $postBlack
              inBlack = $inBlack; tracking = $tracking; svcTag = $svcTag; presenter = $presenter
              loadDesc = $loadDesc; elapsed = $delta.elapsedSec }
}

# ---------------------------------------------------------------- main ------
& (Join-Path $PSScriptRoot 'probe_caps.ps1') -OutDir $outDir
$caps = Get-Content (Join-Path $outDir 'capabilities.json') -Raw | ConvertFrom-Json
$scanoutLuid = ''
if ($caps.scanoutLuid) { $scanoutLuid = $caps.scanoutLuid }
Log ("scanout LUID '" + $scanoutLuid + "'  onAC=" + $caps.power.onAc + "  elevated=" + $caps.elevated)
Log ('tree: ' + $tree)
if ($Sanity) { Log 'SANITY PASS - duty 0, assertions only, no data kept' }

'arm,duty,rep,app_scanout,app_other,svc_scanout,svc_other,dwm_scanout,dwm_other,sys_scanout,sys_other,oth_scanout,oth_other,tot_scanout,tot_other,cpu,presents_s,weaves_s,r_p50_ms,r_p95_ms,r_n,wl_site,r_split,out_crop,post_black,in_black,tracking,svc_tag,presenter,window_s,load_desc,flags' |
    Out-File -Encoding ascii $csvPath

$armDefs = @(
    @{ name = 'render'; env = @{} },
    @{ name = 'split';  env = @{ 'DXR_WEAVE_ON_SCANOUT' = '1' } }
)
$aborted = $false
for ($rep = 1; $rep -le $reps -and -not $aborted; $rep++) {
    foreach ($duty in $duties) {
        if ($aborted) { break }
        foreach ($arm in $armDefs) {            # arms interleaved WITHIN rep
            Log ('cell ' + $arm.name + ' duty=' + $duty + ' rep=' + $rep)
            $r = Invoke-Cell -armName $arm.name -svcEnv $arm.env -duty $duty -rep $rep
            if (-not $r.ok) {
                Log ('  ABORT: ' + $r.abort + '  ' + (($r.flags) -join ';'))
                Log '  This is a launch/assertion failure, not a measurement - stopping.'
                Log '  If it is SPLIT_DID_NOT_ENGAGE on a tree containing D12-0 (#1150),'
                Log '  comp_split_gate is suspect #1 - hand it to the #918 owner.'
                $aborted = $true; break
            }
            $c = $r.col
            $row = ('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19},{20},{21},{22},{23},{24},{25},{26},{27},{28},{29},{30},{31}' -f `
                $arm.name, $duty, $rep, $c.app_scanout, $c.app_other, $c.svc_scanout, $c.svc_other, `
                $c.dwm_scanout, $c.dwm_other, $c.sys_scanout, $c.sys_other, $c.oth_scanout, $c.oth_other, `
                $c.tot_scanout, $c.tot_other, $r.cpu, $r.presents, $r.weaves, $r.rP50, $r.rP95, $r.rN, $r.wlSite, `
                $r.rSplit, $r.rCrop, $r.postBlack, $r.inBlack, $r.tracking, $r.svcTag, $r.presenter, `
                $r.elapsed, $r.loadDesc, (($r.flags) -join ';'))
            if (-not $Sanity) { Add-Content -Path $csvPath -Value $row }
            Log ('  ' + $row)
        }
    }
}

# --- restore the installed service (box-global; peers depend on it) ---------
Stop-Named @($cliBase, 'gpu_loadgen', $svcBase)
$pfSvc = Join-Path ${env:ProgramFiles} 'DisplayXR\Runtime\displayxr-service.exe'
if (Test-Path $pfSvc) {
    Start-Process explorer.exe -ArgumentList $pfSvc | Out-Null
    Start-Sleep -Seconds 5
    $restored = Get-Process -Name $svcBase -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($restored -ne $null -and $restored.Path -eq $pfSvc) { Log ('PF service restored, pid ' + $restored.Id) }
    else { Log 'WARNING: PF service NOT verified restored - restore it manually' }
}
Log ('DONE. ' + $outDir)
