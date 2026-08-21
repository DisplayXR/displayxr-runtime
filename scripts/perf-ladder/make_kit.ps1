# make_kit.ps1 - assemble the exportable DXRPerfLadder kit zip (#1113).
# Bundles the harness (this folder) + a portable avatar build into a flat,
# self-contained folder a partner can unzip and double-click. PS 5.1, ASCII.
#
#   .\make_kit.ps1 -AvatarBuildDir <...\build\windows> -KitVersion 0.1.0 `
#                  [-AvatarRef <git sha/branch>] [-OutDir <dir>]
#
# AvatarBuildDir must be the avatar's build output folder (avatar exe + assets
# + openxr_loader.dll side by side, as build-with-deps.bat leaves it).
param(
    [Parameter(Mandatory = $true)][string]$AvatarBuildDir,
    [string]$KitVersion = '0.1.0',
    [string]$AvatarRef = '',
    [string]$MinRuntime = 'v2.9.0',
    [string]$OutDir = (Join-Path $PSScriptRoot 'dist')
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$exe = Get-ChildItem $AvatarBuildDir -Filter 'avatar_*.exe' | Select-Object -First 1
if ($exe -eq $null) { throw ('no avatar exe under ' + $AvatarBuildDir) }

$kitName = 'DXRPerfLadder-' + $KitVersion
$kitDir = Join-Path $OutDir $kitName
if (Test-Path $kitDir) { Remove-Item $kitDir -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force (Join-Path $kitDir 'avatar') | Out-Null

# Harness (flat at kit root). config-dev.json / results / dist stay behind.
foreach ($f in @('RUN-LADDER.cmd', 'config.json', 'probe_caps.ps1', 'lib_sample.ps1',
                 'run_ladder.ps1', 'summarize.ps1', 'README.md')) {
    Copy-Item (Join-Path $PSScriptRoot $f) $kitDir
}

# Portable avatar: exe + every sibling the build staged next to it (loader dll,
# fbx/textures, manifest, icons). Exclude intermediates.
Get-ChildItem $AvatarBuildDir -File |
    Where-Object { $_.Extension -notin @('.obj', '.pdb', '.lib', '.exp', '.ninja', '.cmake') } |
    Copy-Item -Destination (Join-Path $kitDir 'avatar')

# Fix the app exe name in the shipped config to the actual bundled exe.
$cfgPath = Join-Path $kitDir 'config.json'
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$cfg.app.exe = ('avatar\' + $exe.Name)
foreach ($devKey in @('runtimeJson', 'runtimePath')) {
    if ($cfg.PSObject.Properties[$devKey] -ne $null) { $cfg.PSObject.Properties.Remove($devKey) }
}
$cfg | ConvertTo-Json -Depth 8 | Out-File -Encoding ascii $cfgPath

# Manifest: what this kit IS - results are only comparable per kit version.
$manifest = [ordered]@{
    kit        = 'DXRPerfLadder'
    version    = $KitVersion
    builtUtc   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    avatarExe  = $exe.Name
    avatarRef  = $AvatarRef
    minRuntime = $MinRuntime   # first runtime release carrying DXR_FRAME_WITNESS
}
$manifest | ConvertTo-Json | Out-File -Encoding ascii (Join-Path $kitDir 'kit-manifest.json')

$zip = Join-Path $OutDir ($kitName + '.zip')
if (Test-Path $zip) { Remove-Item $zip -Force -Confirm:$false }
Compress-Archive -Path (Join-Path $kitDir '*') -DestinationPath $zip
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host ('Kit: ' + $zip + '  (' + $mb + ' MB)')
