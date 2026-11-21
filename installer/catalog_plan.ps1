# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Turns `dxr-get catalog --json` into a flat INI file the runtime installer
# (installer/Catalog.nsh) reads with ReadINIStr. NSIS has no JSON parser, and
# this keeps dxr-get's contract plain JSON instead of an installer-specific
# format. Runs under Windows PowerShell 5.1 (and PowerShell 7, for tests).
#
# It only READS: it runs one dxr-get listing command and writes one file. It
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
# What it expects from `dxr-get catalog --json` (the listing a schema-2
# dxr-get prints; see docs/roadmap/catalog-driven-bundle.md). Either an array
# of items or an object with an `items` array. Per item, all optional but `id`:
#   id, aliases[], class, name, summary, version (or target), installed,
#   state (ok | missing | outdated | too_old | not_fetchable | incompatible),
#   size (bytes, the item's asset plus any prerequisite installer assets),
#   detected ("high" | "low" | "none", or true / false), recommended (bool),
#   reason, runtime { min, max }, requires[] (catalog shape; an external
#   entry may carry `met`: false).
# Anything else (text output, a schema-1 dxr-get that has no such verb, a
# usage error) is reported as state=unsupported and the installer behaves as
# it did before this page existed.
#
# Output (ASCII, one section per listed item):
#   [catalog] state=ok|offline|unsupported|error  exit=<dxr-get exit code>
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

# ---- ask dxr-get --------------------------------------------------------------
$dxrArgs = @()
if ($Catalog) { $dxrArgs += @('--catalog', $Catalog) }
$dxrArgs += @('catalog', '--json')

$text = ""
$exit = -1
try {
    $ErrorActionPreference = 'Continue'   # dxr-get logs to stderr; that is not an error
    $text = (& $DxrGet @dxrArgs 2>$null | Out-String)
    $exit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
}
catch {
    Write-Ini 'error' -1 "could not run dxr-get: $($_.Exception.Message)" @() 0 $wantIds
    exit 0
}

if ($exit -eq 10) {
    Write-Ini 'offline' $exit 'The DisplayXR Catalog could not be reached.' @() 0 $wantIds
    exit 0
}
if ($exit -eq 11) {
    Write-Ini 'error' $exit 'The DisplayXR Catalog failed its signature check.' @() 0 @()
    exit 0
}

$doc = $null
$trimmed = $text.Trim()
$isArray = $trimmed.StartsWith('[')
if ($exit -eq 0 -and ($isArray -or $trimmed.StartsWith('{'))) {
    try { $doc = ConvertFrom-Json -InputObject $trimmed } catch { $doc = $null }
}
$raw = $null
if ($null -ne $doc) {
    # A one-element JSON array comes back as a bare object: go by the text.
    if ($isArray) { $raw = @($doc) }
    else { $raw = Get-Prop $doc 'items' }
}
if ($exit -ne 0 -or $null -eq $raw) {
    # A dxr-get without the listing verb (schema 1), or output that is not the listing.
    Write-Ini 'unsupported' $exit 'This dxr-get has no component listing.' @() 0 $wantIds
    exit 0
}

$installing = Parse-Version $RuntimeVersion
$items = New-Object System.Collections.Generic.List[object]
$known = @{}

foreach ($r in @($raw)) {
    $id = Get-Prop $r 'id'
    if (-not $id) { $id = Get-Prop $r 'component' }
    if (-not $id -or ([string]$id) -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { continue }
    $id = [string]$id
    $names = @($id.ToLowerInvariant())
    foreach ($a in @(Get-Prop $r 'aliases')) { if ($a) { $names += ([string]$a).ToLowerInvariant() } }
    foreach ($n in $names) { $known[$n] = $true }

    $class = [string](Get-Prop $r 'class')
    if ($class -eq 'runtime') { continue }                # this installer IS the runtime

    $name = Get-Prop $r 'name'; if (-not $name) { $name = $id }
    $target = Get-Prop $r 'version'; if (-not $target) { $target = Get-Prop $r 'target' }
    $installed = Get-Prop $r 'installed'
    $state = [string](Get-Prop $r 'state')
    $size = 0L
    $sz = Get-Prop $r 'size'
    if ($null -ne $sz) { try { $size = [long]$sz } catch { $size = 0L } }
    $reason = Clean (Get-Prop $r 'reason') 160

    # Status, in the installer's words.
    switch ($state) {
        'ok' { $status = 'current' }
        'missing' { $status = 'missing' }
        'outdated' { $status = 'outdated' }
        'too_old' { $status = 'outdated' }
        'not_fetchable' { $status = 'unavailable'; if (-not $reason) { $reason = 'no signed Windows build is listed' } }
        'incompatible' { $status = 'unavailable'; if (-not $reason) { $reason = 'not compatible with this runtime' } }
        default {
            # No state given: decide from the two versions.
            $iv = Parse-Version $installed; $tv = Parse-Version $target
            if ($null -eq $iv) { $status = 'missing' }
            elseif ($null -ne $tv -and (Compare-Version $iv $tv) -lt 0) { $status = 'outdated' }
            else { $status = 'current' }
        }
    }

    # Runtime range, judged against the runtime THIS installer is laying down
    # (dxr-get judged it against whatever was installed before, possibly none).
    if ($status -ne 'current' -and $null -ne $installing) {
        $lo = $null; $hi = $null
        $rng = Get-Prop $r 'runtime'
        if ($null -ne $rng) { $lo = Parse-Version (Get-Prop $rng 'min'); $hi = Parse-Version (Get-Prop $rng 'max') }
        foreach ($q in @(Get-Prop $r 'requires')) {
            if ((Get-Prop $q 'kind') -eq 'runtime') {
                $f = Parse-Version (Get-Prop $q 'version')
                if ($null -ne $f -and ($null -eq $lo -or (Compare-Version $f $lo) -gt 0)) { $lo = $f }
            }
        }
        if ($null -ne $lo -and (Compare-Version $installing $lo) -lt 0) {
            $status = 'unavailable'; $reason = "needs DisplayXR Runtime $($lo -join '.') or newer"
        }
        elseif ($null -ne $hi -and (Compare-Version $installing $hi) -gt 0) {
            $status = 'unavailable'; $reason = "built for DisplayXR Runtime $($hi -join '.') and not newer"
        }
        elseif ($state -eq 'incompatible' -and ($null -ne $lo -or $null -ne $hi)) {
            # dxr-get called it incompatible with the OLD runtime; it fits the new one.
            $status = if ($installed) { 'outdated' } else { 'missing' }
            $reason = ''
        }
    }

    # Unmet external prerequisites: shown, never a reason to hide the item.
    $unmet = @()
    foreach ($q in @(Get-Prop $r 'requires')) {
        if ((Get-Prop $q 'kind') -eq 'external' -and (Get-Prop $q 'met') -eq $false -and -not (Get-Prop $q 'asset')) {
            $unmet += [string](Get-Prop $q 'name')
        }
    }

    $det = Get-Prop $r 'detected'
    $detected = ($det -eq $true) -or ([string]$det -eq 'high')
    $recommended = (Get-Prop $r 'recommended') -eq $true

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
        'current' { $label = "$name $installed - installed, up to date" }
        'outdated' { $label = "$name $installed to $target ($dl)" }
        'missing' { $label = "$name $target ($dl)" }
        default { $label = "$name - not available" }
    }

    $desc = Clean (Get-Prop $r 'summary') 200
    if ($status -eq 'unavailable') { $desc = "Not available: $reason. $desc" }
    elseif ($status -eq 'current') { $desc = "Already installed at the catalog version; nothing is downloaded. $desc" }
    else {
        if ($detected) { $desc = "Selected because matching hardware was detected on this computer. $desc" }
        elseif ($recommended) { $desc = "Recommended. $desc" }
        if ($unmet.Count -gt 0) { $desc = "$desc Needs, and does not install: $($unmet -join ', ')." }
        $desc = "$desc Downloaded from its publisher during setup."
    }

    $rank = if ($selected) { 0 } elseif ($selectable) { 1 } elseif ($status -eq 'current') { 2 } else { 3 }
    $items.Add(@{
            id = $id; name = (Clean $name 80); label = (Clean $label 120); desc = (Clean $desc 400)
            size_kb = [long][math]::Ceiling($size / 1024); selected = [int]$selected
            ro = [int](-not $selectable); status = $status; rank = ($rank * 100000 + $items.Count)
        })
}

# Requested ids the listing does not know: handed to dxr-get as they are.
$passthrough = @($wantIds | Where-Object { -not $known.ContainsKey($_) })

# rank carries the listing order as a tie-break (Sort-Object -Stable needs PowerShell 6).
$sorted = @($items | Sort-Object -Property @{ Expression = { $_['rank'] } })
$more = 0
if ($sorted.Count -gt $Slots) {
    $more = $sorted.Count - $Slots
    $sorted = @($sorted[0..($Slots - 1)])
}
Write-Ini 'ok' $exit '' $sorted $more $passthrough
exit 0
