# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Turns what dxr-get reports into a flat INI file the runtime installer
# (installer/Catalog.nsh) reads with ReadINIStr. NSIS has no JSON parser, and
# this keeps dxr-get's output plain JSON instead of an installer-specific
# format. Runs under Windows PowerShell 5.1 (and PowerShell 7, for tests).
#
# It only READS: it runs dxr-get's reporting verbs and writes one file. It
# installs nothing and changes no machine state.
#
#   catalog_plan.ps1 -DxrGet <dxr-get.exe> -Out <file.ini> -RuntimeVersion X.Y.Z
#                    [-Slots 8] [-Select <list>] [-Catalog <url|file>]
#
# -Select is the installer's /COMPONENTS= value: a comma-separated list of
# item ids or aliases and the keywords `recommended`, `detected`, `default`
# (= recommended + detected) and `none`. Without it the default pre-ticks
# apply (recommended items, and items whose detection rule matched this
# machine with high confidence).
#
# Three dxr-get calls (docs/specs/runtime/dxr-get.md), joined by item id:
#
#   list --json     {"schema":2,"items":[{id,name,class,installed,available,
#                   state,installable,...}]}. The rows and their state. This
#                   one must succeed; exit 10 is "offline".
#   detect --json   {"edids":[...],"<name>":{match,high_confidence,evidence,id}}
#                   Which items' detection rules match this machine.
#   catalog         the catalog document as fetched. Only a schema-2 document
#                   is used, for what `list` does not carry: `recommended`,
#                   `summary`, `aliases`, the Windows asset's `size`, the
#                   `runtime` range and `requires`.
#
# The second and third are best effort: without them the rows are still
# shown, with nothing pre-ticked by hardware or by recommendation, no size
# and no runtime-range check. docs/roadmap/catalog-driven-bundle.md lists the
# fields that would let `list --json` alone answer all of it.
#
# Output (ASCII, one section per listed item):
#   [catalog] state=ok|offline|error  exit=<exit code of `list`>
#             count=<listed>  more=<not listed: no free slot>
#             passthrough=<requested ids that are not in the listing>
#             message=<one line>
#   [itemN]   id, name, label, desc, size_kb, selected (0|1), ro (0|1),
#             status (missing|outdated|current|unavailable)

param(
    [Parameter(Mandatory = $true)][string]$DxrGet,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$RuntimeVersion = "",
    [int]$Slots = 8,
    [string]$Select = "",
    [string]$Catalog = ""
)

$ErrorActionPreference = 'Stop'
$Platform = 'windows-x64'

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

function Parse-Version([object]$v) {
    # X.Y.Z (a leading 'v' or '>=' is dropped). $null when it is not one.
    if ($null -eq $v) { return $null }
    $m = [regex]::Match([string]$v, '^(?:>=)?v?(\d+)\.(\d+)\.(\d+)')
    if (-not $m.Success) { return $null }
    return , @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value)
}

function Compare-Version([object]$a, [object]$b) {
    for ($i = 0; $i -lt 3; $i++) {
        if ($a[$i] -lt $b[$i]) { return -1 }
        if ($a[$i] -gt $b[$i]) { return 1 }
    }
    return 0
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
        foreach ($k in 'id', 'name', 'label', 'desc', 'size_kb', 'selected', 'ro', 'status') {
            $lines.Add("$k=$(Clean $it[$k])")
        }
    }
    # ASCII: readable by ANSI and Unicode NSIS alike.
    [System.IO.File]::WriteAllLines($Out, $lines.ToArray(), [System.Text.Encoding]::ASCII)
}

function Invoke-DxrGet([string[]]$verb) {
    # stdout as one string, and the exit code. dxr-get logs to stderr.
    $a = @()
    if ($Catalog) { $a += @('--catalog', $Catalog) }
    $a += $verb
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = (& $DxrGet @a 2>$null | Out-String)
        return @{ text = $text.Trim(); exit = $LASTEXITCODE }
    }
    finally { $ErrorActionPreference = $saved }
}

function Read-Json([string]$text) {
    if (-not $text -or -not $text.StartsWith('{')) { return $null }
    try { return ConvertFrom-Json -InputObject $text } catch { return $null }
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
$wantIds = @($tokens | Where-Object { @('recommended', 'detected', 'default', 'none') -notcontains $_ })

# ---- 1. the rows: dxr-get list --json -----------------------------------------
try { $list = Invoke-DxrGet @('list', '--json') }
catch {
    Write-Ini 'error' -1 "could not run dxr-get: $($_.Exception.Message)" @() 0 $wantIds
    exit 0
}
if ($list.exit -eq 10) {
    Write-Ini 'offline' 10 'The DisplayXR Catalog could not be reached.' @() 0 $wantIds
    exit 0
}
if ($list.exit -eq 11) {
    # A refused catalog: nothing is handed to `dxr-get install` either.
    Write-Ini 'error' 11 'The DisplayXR Catalog failed its signature check.' @() 0 @()
    exit 0
}
$rows = Get-Prop (Read-Json $list.text) 'items'
if ($list.exit -ne 0 -or $null -eq $rows) {
    Write-Ini 'error' $list.exit 'dxr-get did not list the catalog.' @() 0 $wantIds
    exit 0
}

# ---- 2. hardware matches: dxr-get detect --json (best effort) ------------------
$detectedHigh = @{}
$legacyName = @{}     # id -> the schema-1 name `detect` keys the item by
$det = Read-Json (Invoke-DxrGet @('detect', '--json')).text
if ($null -ne $det) {
    foreach ($p in $det.PSObject.Properties) {
        $id = Get-Prop $p.Value 'id'
        if (-not $id) { continue }
        $legacyName[[string]$id] = $p.Name.ToLowerInvariant()
        if ((Get-Prop $p.Value 'high_confidence') -eq $true) { $detectedHigh[[string]$id] = $true }
    }
}

# ---- 3. what `list` does not carry: the schema-2 document (best effort) --------
$docItems = @{}
$doc = Read-Json (Invoke-DxrGet @('catalog')).text
if ($null -ne $doc -and (Get-Prop $doc 'schema') -eq 2) {
    foreach ($d in @(Get-Prop $doc 'items')) {
        $id = Get-Prop $d 'id'
        if ($id) { $docItems[[string]$id] = $d }
    }
}

$installing = Parse-Version $RuntimeVersion
$items = New-Object System.Collections.Generic.List[object]
$known = @{}

foreach ($r in @($rows)) {
    $id = [string](Get-Prop $r 'id')
    if ($id -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { continue }
    $d = $docItems[$id]
    $names = @($id.ToLowerInvariant())
    foreach ($a in @(Get-Prop $d 'aliases')) { if ($a) { $names += ([string]$a).ToLowerInvariant() } }
    if ($legacyName.ContainsKey($id)) { $names += $legacyName[$id] }
    foreach ($n in $names) { $known[$n] = $true }

    if ((Get-Prop $r 'class') -eq 'runtime') { continue }      # this installer IS the runtime

    $name = Get-Prop $r 'name'; if (-not $name) { $name = $id }
    $installed = Get-Prop $r 'installed'
    $target = Get-Prop $r 'available'
    $installable = (Get-Prop $r 'installable') -eq $true
    $reason = ''

    # `list` states -> the installer's four.
    switch ([string](Get-Prop $r 'state')) {
        'not_installed' { $status = 'missing' }
        'outdated' { $status = 'outdated' }
        'rebuilt' { $status = 'outdated' }      # same version, another build: `install` re-installs it
        default { $status = 'current' }         # current, newer (never downgraded), installed
    }
    if ($status -ne 'current' -and -not $installable) {
        $status = 'unavailable'
        $reason = if (Get-Prop $r 'listing') { 'it is installed from its publisher''s page' } else { 'no signed Windows build is listed' }
    }

    # Runtime range, judged against the runtime THIS installer is laying down:
    # `dxr-get install` would judge it against that runtime too, but only after
    # the user has committed, and `list` does not judge it at all.
    if (($status -eq 'missing' -or $status -eq 'outdated') -and $null -ne $installing -and $null -ne $d) {
        $rng = Get-Prop $d 'runtime'
        $lo = Parse-Version (Get-Prop $rng 'min')
        $hi = Parse-Version (Get-Prop $rng 'max')
        foreach ($q in @(Get-Prop $d 'requires')) {
            if ((Get-Prop $q 'kind') -eq 'runtime') {
                $f = Parse-Version (Get-Prop $q 'version')
                if ($null -ne $f -and ($null -eq $lo -or (Compare-Version $f $lo) -gt 0)) { $lo = $f }
            }
        }
        if ($null -ne $lo -and (Compare-Version $installing $lo) -lt 0) {
            $status = 'unavailable'; $reason = "it needs DisplayXR Runtime $($lo -join '.') or newer"
        }
        elseif ($null -ne $hi -and (Compare-Version $installing $hi) -gt 0) {
            $status = 'unavailable'; $reason = "it was built for DisplayXR Runtime $($hi -join '.') and not newer"
        }
    }

    # External prerequisites DisplayXR does not install. Whether one is present
    # is only in `plan --for <item>`, one call per item, so it is named, not judged.
    $external = @()
    foreach ($q in @(Get-Prop $d 'requires')) {
        if ((Get-Prop $q 'kind') -eq 'external' -and $null -eq (Get-Prop (Get-Prop $q 'asset') $Platform)) {
            $external += [string](Get-Prop $q 'name')
        }
    }

    $size = 0L
    $sz = Get-Prop (Get-Prop (Get-Prop $d 'assets') $Platform) 'size'
    if ($null -ne $sz) { try { $size = [long]$sz } catch { $size = 0L } }

    $detected = $detectedHigh.ContainsKey($id)
    $recommended = (Get-Prop $d 'recommended') -eq $true

    $selectable = ($status -eq 'missing' -or $status -eq 'outdated')
    $selected = $false
    if ($selectable) {
        if ($wantRecommended -and $recommended) { $selected = $true }
        if ($wantDetected -and $detected) { $selected = $true }
        foreach ($n in $names) { if ($wantIds -contains $n) { $selected = $true } }
    }

    $sizeText = Format-Size $size
    $dl = if ($sizeText) { "download $sizeText" } else { 'download' }
    switch ($status) {
        'current' { $label = "$name $installed - installed" }
        'outdated' { $label = "$name $installed to $target ($dl)" }
        'missing' { $label = "$name $target ($dl)" }
        default { $label = "$name - not available" }
    }

    $desc = Clean (Get-Prop $d 'summary') 200
    if ($status -eq 'unavailable') { $desc = "Not available: $reason. $desc" }
    elseif ($status -eq 'current') { $desc = "Already installed; nothing is downloaded. $desc" }
    else {
        if ($detected) { $desc = "Selected because matching hardware was detected on this computer. $desc" }
        elseif ($recommended) { $desc = "Recommended. $desc" }
        if ($external.Count -gt 0) { $desc = "$desc Also needs, and does not install: $($external -join ', ')." }
        $desc = "$desc Downloaded from its publisher during setup."
    }

    $rank = if ($selected) { 0 } elseif ($selectable) { 1 } elseif ($status -eq 'current') { 2 } else { 3 }
    $items.Add(@{
            id = $id; name = (Clean $name 80); label = (Clean $label 120); desc = (Clean $desc 400)
            size_kb = [long][math]::Ceiling($size / 1024); selected = [int]$selected
            ro = [int](-not $selectable); status = $status
            rank = ($rank * 100000 + $items.Count)   # the listing order breaks ties (no Sort -Stable in 5.1)
        })
}

# Requested ids the listing does not know: handed to dxr-get as they are (it
# resolves aliases itself, and answers 40 for an id it does not know).
$passthrough = @($wantIds | Where-Object { -not $known.ContainsKey($_) })

$sorted = @($items | Sort-Object -Property @{ Expression = { $_['rank'] } })
$more = 0
if ($sorted.Count -gt $Slots) {
    $more = $sorted.Count - $Slots
    $sorted = @($sorted[0..($Slots - 1)])
}
Write-Ini 'ok' 0 '' $sorted $more $passthrough
exit 0
