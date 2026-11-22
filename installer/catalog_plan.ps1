# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Turns `dxr-get list --json` into a flat INI file the runtime installer
# (installer/Catalog.nsh) reads with ReadINIStr. NSIS has no JSON parser, and
# this keeps dxr-get's output plain JSON instead of an installer-specific
# format. Runs under Windows PowerShell 5.1 (and PowerShell 7, for tests).
#
# It only READS: it runs one dxr-get listing command and writes one file. It
# installs nothing and changes no machine state. It also decides nothing about
# compatibility, detection or sizes: those are dxr-get's answers, copied.
#
#   catalog_plan.ps1 -DxrGet <dxr-get.exe> -Out <file.ini> -RuntimeVersion X.Y.Z
#                    [-AssumeAbi class=N,class=N] [-Slots 8] [-Select <list>]
#                    [-Catalog <url|file>]
#
# The one call (docs/specs/runtime/dxr-get.md, "list" and "The runtime installer"):
#
#   dxr-get [--catalog X] list --json --assume-runtime <RuntimeVersion>
#           [--assume-abi <class>=<major> ...]
#
# --assume-runtime makes dxr-get answer for the runtime this installer is about
# to lay down, not the one on the machine; -AssumeAbi carries the class ABI
# majors of that runtime (the installer writes the same numbers to Runtime\Abi).
# With an assumed runtime the runtime is taken as fixed, so an item that needs
# a newer one comes back `compatible: false`.
#
# -Select is the installer's /COMPONENTS= value: a comma-separated list of
# item ids or aliases and the keywords `recommended`, `detected`, `default`
# (= recommended + detected), `installed` and `none`. Without it the default
# pre-ticks apply: `recommended` items, items with `detected: "high"`, and
# items that are ALREADY INSTALLED and have an update.
#
# Already installed => ticked, for every item, whatever its class: a component
# the user has is never presented as something they are about to lose.
#   installed, older than the catalog  -> `selected=1` by default (the update is
#       installed with the runtime); unticking it only skips the update.
#   installed, at the catalog version  -> `installed=1`, `ro=1`: the page shows
#       it ticked and greyed. `selected` stays 0: nothing is downloaded or run.
# Nothing on this page ever REMOVES a component, ticked or not.
# `default` deliberately stays recommended + detected, so the page's meaning of
# it equals `dxr-get install --default` (the unattended one-call path).
#
# Output (ASCII, one section per listed item):
#   [catalog] state=ok|offline|error  exit=<dxr-get exit code>
#             count=<listed>  more=<not listed: no free slot>
#             passthrough=<requested ids the listing lacks or marks unavailable>
#             message=<one line>
#   [itemN]   id, name, label, desc, size_kb, selected (0|1), ro (0|1),
#             installed (0|1), status (missing|outdated|current|unavailable)
#             selected = dxr-get is asked to install or update it.
#             installed = it is on this machine now (current or outdated).

param(
    [Parameter(Mandatory = $true)][string]$DxrGet,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$RuntimeVersion = "",
    [string]$AssumeAbi = "",
    [int]$Slots = 8,
    [string]$Select = "",
    [string]$Catalog = ""
)

$ErrorActionPreference = 'Stop'

function Clean([object]$v, [int]$max = 400) {
    # One INI line, ASCII: no control characters, no line breaks.
    if ($null -eq $v) { return "" }
    $s = [string]$v
    $s = [regex]::Replace($s, '[\x00-\x1F\x7F]+', ' ')
    $s = [regex]::Replace($s, '[^\x20-\x7E]', '?').Trim()
    if ($s.Length -gt $max) { $s = $s.Substring(0, $max - 3) + '...' }
    return $s
}

function Get-Prop([object]$o, [string]$name) {
    if ($null -eq $o) { return $null }
    $p = $o.PSObject.Properties[$name]
    if ($null -eq $p) { return $null }
    return $p.Value
}

function To-Long([object]$v) {
    if ($null -eq $v) { return 0L }
    try { return [long]$v } catch { return 0L }
}

function Format-Size([long]$bytes) {
    if ($bytes -le 0) { return "" }
    if ($bytes -ge 1GB) { return ("{0:0.0} GB" -f ($bytes / 1GB)) }
    $mb = [math]::Max(1, [math]::Round($bytes / 1MB))
    return "$mb MB"
}

$lines = New-Object System.Collections.Generic.List[string]
function Write-Ini([string]$state, [int]$exit, [string]$message, [object[]]$items, [int]$more, [string[]]$passthrough) {
    $lines.Add("[catalog]")
    $lines.Add("state=$state")
    $lines.Add("exit=$exit")
    $lines.Add("count=$($items.Count)")
    $lines.Add("more=$more")
    $lines.Add("passthrough=$(($passthrough | Where-Object { $_ }) -join ' ')")
    $lines.Add("message=$(Clean $message)")
    $n = 0
    foreach ($it in $items) {
        $n++
        $lines.Add("[item$n]")
        foreach ($k in 'id', 'name', 'label', 'desc', 'size_kb', 'selected', 'ro', 'installed', 'status') {
            $lines.Add("$k=$(Clean $it[$k])")
        }
    }
    # ASCII: readable by ANSI and Unicode NSIS alike.
    [System.IO.File]::WriteAllLines($Out, $lines.ToArray(), [System.Text.Encoding]::ASCII)
}

# ---- the requested selection ------------------------------------------------
$tokens = @()
if ($Select) {
    $tokens = @($Select.Split(',') | ForEach-Object { $_.Trim().ToLowerInvariant() } |
            Where-Object { $_ -match '^[a-z0-9][a-z0-9._-]*$' })
}
$explicit = $tokens.Count -gt 0
$wantRecommended = (-not $explicit) -or ($tokens -contains 'recommended') -or ($tokens -contains 'default')
$wantDetected = (-not $explicit) -or ($tokens -contains 'detected') -or ($tokens -contains 'default')
$wantInstalled = (-not $explicit) -or ($tokens -contains 'installed')
$wantIds = @($tokens | Where-Object { @('recommended', 'detected', 'default', 'installed', 'none') -notcontains $_ })

# ---- dxr-get list --json --assume-runtime ... ---------------------------------
$dxrArgs = @()
if ($Catalog) { $dxrArgs += @('--catalog', $Catalog) }
$dxrArgs += @('list', '--json')
if ($RuntimeVersion -match '^\d+\.\d+\.\d+$') {
    $dxrArgs += @('--assume-runtime', $RuntimeVersion)
    foreach ($pair in $AssumeAbi.Split(',')) {
        if ($pair -match '^[a-z_]+=\d+$') { $dxrArgs += @('--assume-abi', $pair) }
    }
}

$text = ""
$exit = -1
try {
    $ErrorActionPreference = 'Continue'   # dxr-get logs to stderr; that is not an error
    $text = (& $DxrGet @dxrArgs 2>$null | Out-String).Trim()
    $exit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
}
catch {
    Write-Ini 'error' -1 "could not run dxr-get: $($_.Exception.Message)" @() 0 $wantIds
    exit 0
}

if ($exit -eq 10) {
    Write-Ini 'offline' 10 'The DisplayXR Catalog could not be reached.' @() 0 $wantIds
    exit 0
}
if ($exit -eq 11) {
    # A refused catalog: nothing is handed to `dxr-get install` either.
    Write-Ini 'error' 11 'The DisplayXR Catalog failed its signature check.' @() 0 @()
    exit 0
}
$rows = $null
if ($exit -eq 0 -and $text.StartsWith('{')) {
    try { $rows = Get-Prop (ConvertFrom-Json -InputObject $text) 'items' } catch { $rows = $null }
}
if ($null -eq $rows) {
    Write-Ini 'error' $exit 'dxr-get did not list the catalog.' @() 0 $wantIds
    exit 0
}

# The description box under the list is two lines of about 50 characters
# (measured on Windows). 95 leaves room for the word wrap: a line breaks at a
# space, so two lines hold less than 2 x 50 when a long word moves down.
$DescMax = 95

# A row of the component list. Measured on Windows (the page layout with the
# description under the list, 100 % scaling): a 53-character row fits
# ("Update, 1 MB - Leia display plug-in 2.23.0 to 2.23.1"), and a 59-character
# one is cut after its 52nd character and brings the horizontal scrollbar back.
# The font is proportional, so the limit is about 52 characters of ordinary
# text, not a fixed count: 50 leaves two characters for wider letters. The
# longest first-party row today, a 17-character status, the 25-character name
# and a version of the form N.NN.NN, is exactly 50:
# "Update, cached - Leia SR display processor 2.23.11" (and the same length
# with "Update, 115 MB - " or "Update, 1.2 GB - "). Anything longer (a longer
# third-party name, a two-digit major version on that name) has its NAME cut.
$LabelMax = 50

function Fit-Label([string]$status, [string]$name, [string]$tail) {
    # status + name + tail, at most $LabelMax characters. The status and the
    # version stay whole; a name that does not fit is cut and ends in "...".
    $room = $LabelMax - $status.Length - $tail.Length
    if ($name.Length -gt $room) {
        if ($room -lt 8) { $room = 8 }       # a name is never cut to nothing
        $name = $name.Substring(0, $room - 3).TrimEnd(' ', '-', ',', '.', ':') + '...'
    }
    return "$status$name$tail"
}

function Clip-Desc([string]$text) {
    # A description that fits is left alone. A longer one is cut at the last
    # word that fits and ends in "...", so the marker never stands alone on a
    # third, hidden line and no word is cut in half.
    $s = Clean $text 400
    if ($s.Length -le $DescMax) { return $s }
    $cut = $s.Substring(0, $DescMax - 3)
    $sp = $cut.LastIndexOf(' ')
    if ($sp -ge ($DescMax - 3 - 20)) { $cut = $cut.Substring(0, $sp) }
    return $cut.TrimEnd(' ', ',', ';', ':', '.', '-', '(') + '...'
}
$items = New-Object System.Collections.Generic.List[object]
$known = @{}
$refused = New-Object System.Collections.Generic.List[string]

foreach ($r in @($rows)) {
    $id = [string](Get-Prop $r 'id')
    if ($id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { continue }
    $names = @($id.ToLowerInvariant())
    foreach ($a in @(Get-Prop $r 'aliases')) { if ($a) { $names += ([string]$a).ToLowerInvariant() } }
    foreach ($n in $names) { $known[$n] = $true }

    if ((Get-Prop $r 'class') -eq 'runtime') { continue }      # this installer IS the runtime

    $name = Get-Prop $r 'name'; if (-not $name) { $name = $id }
    $installed = Get-Prop $r 'installed'
    $target = Get-Prop $r 'available'
    $reason = ''

    # `list` states -> the installer's four.
    switch ([string](Get-Prop $r 'state')) {
        'not_installed' { $status = 'missing' }
        'outdated' { $status = 'outdated' }
        'rebuilt' { $status = 'outdated' }      # same version, another build: `install` re-installs it
        default { $status = 'current' }         # current, newer (never downgraded), installed
    }
    if ($status -ne 'current') {
        if ((Get-Prop $r 'installable') -ne $true) {
            $status = 'unavailable'
            $reason = if (Get-Prop $r 'listing') { 'it is installed from its publisher''s page' } else { 'no signed Windows build is listed' }
        }
        elseif ((Get-Prop $r 'compatible') -eq $false) {
            # dxr-get's verdict for the runtime being installed (range and class ABI).
            $status = 'unavailable'
            $reason = Clean (Get-Prop $r 'incompatible_reason') 150
            if (-not $reason) { $reason = 'it is not compatible with this version of the DisplayXR Runtime' }
        }
    }
    # Named in -Select but not offered here (not compatible, no signed build, a
    # link-out): never dropped silently. It is handed to `dxr-get install` like
    # an unknown id, so the engine's own refusal (41, 40) is what gets reported.
    # The row itself stays read-only and unticked.
    if ($status -eq 'unavailable' -and -not $refused.Contains($id)) {
        foreach ($n in $names) { if ($wantIds -contains $n) { $refused.Add($id); break } }
    }

    # External prerequisites DisplayXR does not install, and that are not there.
    $unmet = @()
    foreach ($q in @(Get-Prop $r 'prerequisites')) {
        if ($null -ne $q -and (Get-Prop $q 'met') -eq $false -and (Get-Prop $q 'installable') -ne $true) {
            $unmet += [string](Get-Prop $q 'name')
        }
    }

    $detected = ([string](Get-Prop $r 'detected')) -eq 'high'
    $recommended = (Get-Prop $r 'recommended') -eq $true

    $selectable = ($status -eq 'missing' -or $status -eq 'outdated')
    # On this machine now. An unavailable row can be an installed item whose
    # update cannot be installed here: it stays installed, and is shown so.
    $isInstalled = [bool]$installed
    $keptForUpdate = $false
    $selected = $false
    if ($selectable) {
        if ($wantRecommended -and $recommended) { $selected = $true }
        if ($wantDetected -and $detected) { $selected = $true }
        # Already installed => ticked: the update comes with the runtime.
        if ($wantInstalled -and $status -eq 'outdated' -and -not $selected) { $selected = $true; $keptForUpdate = $true }
        foreach ($n in $names) { if ($wantIds -contains $n) { $selected = $true } }
    }

    # What `install <id>` would still fetch: the asset plus the installers of
    # unmet prerequisites, minus the download cache. 0 = already downloaded.
    $download = To-Long (Get-Prop $r 'download')
    $sizeText = Format-Size $download
    # The status comes FIRST: the list is narrow and cuts a row on the right,
    # and what must stay readable without scrolling is the status.
    # Every row must also FIT the list: a row wider than the list is cut and
    # brings the horizontal scrollbar back (see $LabelMax). So the row carries
    # one version, the one it ends up with; the version an update replaces is
    # in the description. Nothing left to fetch reads "cached".
    $nm = Clean $name 80
    switch ($status) {
        'current' { $label = Fit-Label 'Installed - ' $nm " $installed" }
        'outdated' { $label = Fit-Label $(if ($sizeText) { "Update, $sizeText - " } else { 'Update, cached - ' }) $nm " $target" }
        'missing' { $label = Fit-Label $(if ($sizeText) { "$sizeText - " } else { 'Cached - ' }) $nm " $target" }
        default { $label = Fit-Label 'Not available - ' $nm '' }
    }

    # The description box holds about two lines: what matters first, and the
    # whole text short (see $DescMax below).
    $desc = Clean (Get-Prop $r 'summary') 200
    if ($status -eq 'unavailable') {
        # The row already says "Not available": when the reason is long, it
        # alone gets the two lines.
        $desc = "Not available: $reason."
        if ($desc.Length -gt $DescMax) { $desc = "$reason." }
    }
    elseif ($status -eq 'current') { $desc = "Already installed; nothing is downloaded. $desc" }
    else {
        if ($unmet.Count -gt 0) { $desc = "Also needs, and does not install: $($unmet -join ', ') (not found). $desc" }
        if ($detected) { $desc = "Matching hardware was detected. $desc" }
        elseif ($recommended) { $desc = "Recommended. $desc" }
        # The row shows the new version only; the one it replaces is here.
        if ($keptForUpdate) { $desc = "Updates the installed $installed. Clear the box to keep that version. $desc" }
        elseif ($status -eq 'outdated') { $desc = "Updates the installed $installed. $desc" }
    }

    # Which rows are kept when there are more items than slots: ticked, then
    # what can be ticked, then installed, then not available (dropped first).
    $rank = if ($selected) { 0 } elseif ($selectable) { 1 } elseif ($status -eq 'current') { 2 } else { 3 }
    $items.Add(@{
            id = $id; name = (Clean $name 80); label = (Clean $label $LabelMax); desc = (Clip-Desc $desc)
            size_kb = [long][math]::Ceiling($download / 1024); selected = [int]$selected
            ro = [int](-not $selectable); installed = [int]$isInstalled; status = $status
            rank = ($rank * 100000 + $items.Count)   # the listing order breaks ties (no Sort -Stable in 5.1)
        })
}

# Requested ids the listing does not know: handed to dxr-get as they are (it
# answers 40 for an id it does not know). Then the requested items it refuses.
$passthrough = @($wantIds | Where-Object { -not $known.ContainsKey($_) }) + @($refused.ToArray())

$sorted = @($items | Sort-Object -Property @{ Expression = { $_['rank'] } })
$more = 0
if ($sorted.Count -gt $Slots) {
    $more = $sorted.Count - $Slots
    $sorted = @($sorted[0..($Slots - 1)])
}
Write-Ini 'ok' 0 '' $sorted $more $passthrough
exit 0
