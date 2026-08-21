# run_ladder.ps1 - DXR perf-decomposition ladder runner (issue #1113).
# Runs the arm table from config.json: reps are INTERLEAVED (full pass over all
# arms, repeated), each sample is one 20 s GPU Running-Time delta window after
# warmup. Non-elevated by design; writes only under -OutRoot.
# PowerShell 5.1, ASCII only.
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot 'config.json'),
    [string]$OutRoot = (Join-Path $PSScriptRoot 'results'),
    [string]$AppExeOverride = '',
    [string[]]$Arms = @(),        # subset filter, e.g. -Arms IDLE-P,SHIP
    [int]$RepsOverride = 0,
    [switch]$Smoke                # 1 rep, 8 s windows, 2 s warmup
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

# powershell.exe -File passes "-Arms A,B" as one string - normalize.
if ($Arms.Count -eq 1 -and $Arms[0] -match ',') { $Arms = $Arms[0].Split(',') }

$cfg = Get-Content $ConfigPath -Raw | ConvertFrom-Json
$windowSec = $cfg.windowSeconds
$warmupSec = $cfg.warmupSeconds
$reps = $cfg.reps
if ($Smoke) { $windowSec = 8; $warmupSec = 2; $reps = 1 }
if ($RepsOverride -gt 0) { $reps = $RepsOverride }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $OutRoot ($env:COMPUTERNAME + '-' + $stamp)
New-Item -ItemType Directory -Force $outDir | Out-Null
$csvPath = Join-Path $outDir 'ladder.csv'
$runLog  = Join-Path $outDir 'run.log'

function Log { param([string]$m)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
    Write-Host $line
    Add-Content -Path $runLog -Value $line
}

# --- Block 0: capabilities ---------------------------------------------------
& (Join-Path $PSScriptRoot 'probe_caps.ps1') -OutDir $outDir
$caps = Get-Content (Join-Path $outDir 'capabilities.json') -Raw | ConvertFrom-Json
$scanoutLuid = ''
if ($caps.scanoutLuid) { $scanoutLuid = $caps.scanoutLuid }
Log ("scanout LUID: '" + $scanoutLuid + "'  onAC=" + $caps.power.onAc + "  elevated=" + $caps.elevated)
if (-not $caps.power.onAc) { Log 'FLAG: on battery - results will be invalid; plug in AC.' }

# --- App exe -----------------------------------------------------------------
$appExe = $cfg.app.exe
if ($AppExeOverride -ne '') { $appExe = $AppExeOverride }
if (-not [IO.Path]::IsPathRooted($appExe)) { $appExe = Join-Path (Split-Path $PSScriptRoot -Parent) $appExe }
$needApp = $false
foreach ($a in $cfg.arms) { if ($a.app) { $needApp = $true } }
if ($Arms.Count -gt 0) {
    $needApp = $false
    foreach ($a in $cfg.arms) { if (($Arms -contains $a.name) -and $a.app) { $needApp = $true } }
}
if ($needApp -and -not (Test-Path $appExe)) {
    throw ('App exe not found: ' + $appExe + '  (pass -AppExeOverride or fix config.json)')
}
$exeBase = [IO.Path]::GetFileNameWithoutExtension($appExe)

# --- Screen geometry for placement + orbit -----------------------------------
Add-Type -AssemblyName System.Windows.Forms
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$orbitCx = [int]($screen.Width / 2)
$orbitCy = [int]($screen.Height / 2)

'arm,rep,app_scanout,app_other,dwm,other_gpu,total_gpu,app_cpu,presents_s,weaves_s,repaints_s,mode,window_s,flags' |
    Out-File -Encoding ascii $csvPath

function Invoke-ArmSample {
    param($arm, [int]$rep)
    $flags = @()
    $appPids = @()
    $appProc = $null
    $batPath = $null
    $appLogBefore = Get-LatestDxrLog $exeBase

    if ($arm.app) {
        # Compose env into a launcher bat (env must be process-level; explorer
        # launch strips elevation so the loader honors the registry runtime).
        $batPath = Join-Path $env:TEMP ('ladder_arm_' + $arm.name + '.bat')
        $lines = @('@echo off')
        # Dev-only: point the loader at a dev runtime json (the shipped kit
        # omits this and uses the installed/registry runtime).
        $devJson = $null
        if ($cfg.PSObject.Properties['runtimeJson'] -ne $null) { $devJson = $cfg.runtimeJson }
        if (-not [string]::IsNullOrEmpty($devJson)) { $lines += ('set "XR_RUNTIME_JSON={0}"' -f $devJson) }
        foreach ($kv in $cfg.baseEnv.PSObject.Properties) { $lines += ('set "{0}={1}"' -f $kv.Name, $kv.Value) }
        foreach ($kv in $arm.env.PSObject.Properties)     { $lines += ('set "{0}={1}"' -f $kv.Name, $kv.Value) }
        if ($cfg.app.windowSize) { $lines += ('set "DXR_AVATAR_WINDOW={0}"' -f $cfg.app.windowSize) }
        $lines += ('"{0}" {1} > "%TEMP%\ladder_arm_{2}.out" 2>&1' -f $appExe, $cfg.app.args, $arm.name)
        Set-Content -Path $batPath -Value ($lines -join "`r`n") -Encoding Ascii

        Start-Process explorer.exe -ArgumentList $batPath | Out-Null
        # Find the app process (child of the bat's cmd, so poll by exe name + start time).
        $t0 = Get-Date
        while (((Get-Date) - $t0).TotalSeconds -lt $cfg.app.startupTimeoutSec) {
            $appProc = Get-Process -Name $exeBase -ErrorAction SilentlyContinue |
                       Where-Object { $_.StartTime -gt $t0.AddSeconds(-2) } | Select-Object -First 1
            if ($appProc -ne $null) { break }
            Start-Sleep -Milliseconds 500
        }
        if ($appProc -eq $null) { $flags += 'APP_NO_START'; return @{ ok = $false; flags = $flags } }
        $appPids = @($appProc.Id)

        $hwnd = Get-MainWindow -procId $appProc.Id -timeoutSec 15
        if ($hwnd -ne [IntPtr]::Zero) {
            Move-WindowTo -hwnd $hwnd -x ([int](($screen.Width - 600) / 2)) -y ([int](($screen.Height - 1051) / 2))
        } else { $flags += 'NO_HWND' }

        # Mode set: closed loop on the witness (V toggles; witness verifies).
        if ($arm.mode -ne $null -and $hwnd -ne [IntPtr]::Zero) {
            $deadline = (Get-Date).AddSeconds(40)
            $settled = $false
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Seconds 6   # one witness interval
                $wl = @(Get-WitnessLines (Get-LatestDxrLog $exeBase)) | Select-Object -Last 1
                if ($wl -ne $null -and $wl.Mode -eq $arm.mode) { $settled = $true; break }
                if ($wl -ne $null) { Send-KeyToWindow -hwnd $hwnd -vk 0x56 }  # 'V'
            }
            if (-not $settled) { $flags += ('MODE_UNSETTLED_want_' + $arm.mode) }
        }
    }

    # Cursor state
    $orbitProc = $null
    if ($arm.cursor -eq 'orbit') {
        $orbitProc = Start-CursorOrbit -centerX $orbitCx -centerY $orbitCy `
            -radiusPx $cfg.cursorOrbit.radiusPx -hz $cfg.cursorOrbit.hz `
            -durationSec ($warmupSec + $windowSec + 10)
    } else {
        Set-CursorParked
    }

    Start-Sleep -Seconds $warmupSec
    $witBefore = @(Get-WitnessLines (Get-LatestDxrLog $exeBase)).Count

    $cpu0 = Get-CpuSnapshot -procIds $appPids
    $g0 = Get-GpuSnapshot
    if (-not $g0.ok) { $flags += 'GPU_COUNTER_FAIL' }
    Start-Sleep -Seconds $windowSec
    $g1 = Get-GpuSnapshot
    $cpu1 = Get-CpuSnapshot -procIds $appPids

    $wit = @()
    if ($arm.app) {
        # Witness lines that landed during the sample window (skip pre-window ones).
        $all = @(Get-WitnessLines (Get-LatestDxrLog $exeBase))
        if ($all.Count -gt $witBefore) { $wit = $all[$witBefore..($all.Count - 1)] }
        if ($wit.Count -eq 0) { $flags += 'NO_WITNESS' }
    }

    if ($orbitProc -ne $null) { Stop-Process -Id $orbitProc.Id -Force -ErrorAction SilentlyContinue }
    if ($appProc -ne $null) { Stop-Process -Id $appProc.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2 }

    $delta = Get-GpuDelta $g0 $g1
    if (-not $delta.ok) { $flags += 'GPU_DELTA_FAIL'; return @{ ok = $false; flags = $flags } }
    $col = Resolve-GpuColumns -deltaRows $delta.rows -appPids $appPids -scanoutLuid $scanoutLuid
    $appCpu = Get-CpuDeltaPct $cpu0 $cpu1

    $wp = 0.0; $ww = 0.0; $wr = 0.0; $wm = ''
    if ($wit.Count -gt 0) {
        $wp = Get-TrueMedian @($wit | ForEach-Object { $_.PresentsPerS })
        $ww = Get-TrueMedian @($wit | ForEach-Object { $_.WeavesPerS })
        $wr = Get-TrueMedian @($wit | ForEach-Object { $_.RepaintsPerS })
        $wm = ($wit | Select-Object -Last 1).Mode
    }

    $row = ('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13}' -f `
        $arm.name, $rep, $col.app_scanout, $col.app_other, $col.dwm, $col.other, $col.total, `
        $appCpu, $wp, $ww, $wr, $wm, $delta.elapsedSec, ($flags -join ';'))
    Add-Content -Path $csvPath -Value $row
    Log ('  ' + $row)
    return @{ ok = $true; flags = $flags }
}

# --- Main loop: interleaved reps --------------------------------------------
$armList = @($cfg.arms)
if ($Arms.Count -gt 0) { $armList = @($cfg.arms | Where-Object { $Arms -contains $_.name }) }
Log ('Ladder start: ' + $armList.Count + ' arms x ' + $reps + ' reps, window ' + $windowSec + ' s, warmup ' + $warmupSec + ' s')

for ($rep = 1; $rep -le $reps; $rep++) {
    foreach ($arm in $armList) {
        Log ('arm ' + $arm.name + ' rep ' + $rep)
        try {
            $r = Invoke-ArmSample -arm $arm -rep $rep
            if (-not $r.ok) { Log ('  FAILED: ' + ($r.flags -join ';')) }
        } catch {
            Log ('  ERROR: ' + $_.Exception.Message)
            Get-Process -Name $exeBase -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Seconds 3   # settle between arms
    }
}

# --- Harvest runtime logs + summarize ---------------------------------------
$logDir = Join-Path $env:LOCALAPPDATA 'DisplayXR'
if (Test-Path $logDir) {
    $dst = Join-Path $outDir 'dxr-logs'
    New-Item -ItemType Directory -Force $dst | Out-Null
    Get-ChildItem $logDir -Filter '*.log' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-3) } |
        Copy-Item -Destination $dst -ErrorAction SilentlyContinue
}
& (Join-Path $PSScriptRoot 'summarize.ps1') -ResultsDir $outDir

$zip = Join-Path $OutRoot ('ladder-results-' + $env:COMPUTERNAME + '-' + $stamp + '.zip')
Compress-Archive -Path (Join-Path $outDir '*') -DestinationPath $zip -Force
Log ('DONE. Send back: ' + $zip)
