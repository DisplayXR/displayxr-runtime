<#
.SYNOPSIS
  Provision Mesa's software rasterizers (llvmpipe for OpenGL, lavapipe for
  Vulkan) on a machine that has no GPU driver, so the OpenXR CTS `opengl`,
  `vulkan` and `vulkan2` arms can run at all. (#1525, epic #1523)

.DESCRIPTION
  A GitHub-hosted `windows-2022` runner has:

    * OpenGL — only the GDI generic **OpenGL 1.1** implementation in
      System32\opengl32.dll. The CTS GL plugin needs a modern core profile, so
      `-G opengl` cannot start.
    * Vulkan — a loader (vcpkg `vulkan-loader`) but **no ICD**, so
      vkEnumeratePhysicalDevices returns zero devices and `-G vulkan` /
      `-G vulkan2` cannot start.

  This script fixes both from ONE pinned artifact:

    * **llvmpipe** — Mesa's `opengl32.dll` is a small ICD shim that dispatches
      into `libgallium_wgl.dll`. Windows resolves an implicit `opengl32.dll`
      import from the **application directory** before System32 (opengl32 is
      NOT a KnownDLL on Windows 10/11), so dropping the pair beside an .exe
      redirects that process — and every DLL it loads — onto Mesa without
      touching the machine.
    * **lavapipe** — `vulkan_lvp.dll` + `lvp_icd.x86_64.json`, registered with
      the Vulkan loader purely through `VK_DRIVER_FILES`. Nothing is installed
      and no registry key is written.

  **Why lavapipe and not SwiftShader.** SwiftShader has the longer Vulkan
  conformance track record, but it publishes no pinned prebuilt Windows binary
  — adopting it means building it, i.e. a second toolchain and a second thing
  to pin. lavapipe arrives in the same archive as llvmpipe, so the whole
  software tier is ONE version number and ONE SHA256, and a Mesa bump moves the
  GL and VK arms together instead of letting them drift apart. This tier exists
  to catch API-contract regressions, not to produce submission artefacts
  (see #1523), so breadth of conformance pedigree is not what is being bought.

  **Provenance.** Mesa has no official Windows binary release; `pal1000/
  mesa-dist-win` is the long-running community build of upstream Mesa and is
  what Mesa's own docs point Windows users at. The archive is pinned by exact
  version AND SHA256 — never "latest" — and the checksum is verified before
  anything is unpacked.

.PARAMETER Version   mesa-dist-win release tag (exact; never a floating ref).
.PARAMETER Sha256    Expected SHA256 of the release .7z. MUST match the Version.
.PARAMETER CacheDir  Where the archive + unpacked tree live. Cache this
                     directory between runs; the script re-downloads only when
                     the pinned archive is absent or fails its checksum.
.PARAMETER StageTo   Directories that receive `opengl32.dll` +
                     `libgallium_wgl.dll` (and `vulkan-1.dll`, if -VulkanLoaderDll
                     is given). Pass the CTS executable's directory at minimum.
.PARAMETER Unstage   Remove everything a previous -StageTo run copied, using the
                     manifest it left behind. Run this before any step that
                     caches a staged directory — otherwise the next job restores
                     a build tree with Mesa's opengl32.dll silently shadowing
                     the system one, and an arm that never asked for software
                     rendering gets it anyway.
.PARAMETER GithubEnv Append VK_DRIVER_FILES / GALLIUM_DRIVER / the version
                     stamp to $GITHUB_ENV for subsequent workflow steps.

.EXAMPLE
  scripts\fetch_mesa_rasterizers.ps1 -StageTo build-cts\...\RelWithDebInfo -GithubEnv
  scripts\fetch_mesa_rasterizers.ps1 -Unstage
#>
param(
  [string]  $Version  = "26.1.8",
  [string]  $Sha256   = "4c6d32e653e0ff9ad07796e40c0bcfabf2764d849e3ce4f3b1590112c87e42f9",
  [string]  $CacheDir = "",
  [string[]]$StageTo  = @(),
  [switch]  $Unstage,
  [switch]  $GithubEnv
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $CacheDir) { $CacheDir = Join-Path $repo "mesa-sw-gfx" }

# The GL runtime pair. opengl32.dll is only the dispatch shim — without
# libgallium_wgl.dll beside it, wglCreateContext fails and the CTS reports a
# context-creation error that looks nothing like "missing DLL".
$glRuntime = @("opengl32.dll", "libgallium_wgl.dll")

$manifest = Join-Path $CacheDir "staged_manifest.txt"

# ---------------------------------------------------------------- unstage ----
if ($Unstage) {
  if (-not (Test-Path $manifest)) {
    Write-Host "mesa: nothing staged (no manifest at $manifest)"
    exit 0
  }
  $n = 0
  foreach ($p in (Get-Content $manifest | Where-Object { $_.Trim() })) {
    if (Test-Path $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue; $n++ }
  }
  Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
  Write-Host "mesa: unstaged $n file(s)"
  exit 0
}

# --------------------------------------------------------------- download ----
$asset   = "mesa3d-$Version-release-msvc.7z"
$url     = "https://github.com/pal1000/mesa-dist-win/releases/download/$Version/$asset"
$archive = Join-Path $CacheDir $asset
$unpack  = Join-Path $CacheDir "mesa-$Version"
$x64     = Join-Path $unpack "x64"

New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

function Get-Sha256([string]$Path) {
  return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$needDownload = $true
if (Test-Path $archive) {
  $have = Get-Sha256 $archive
  if ($have -eq $Sha256.ToLowerInvariant()) {
    Write-Host "mesa: cached archive checksum OK ($asset)"
    $needDownload = $false
  } else {
    Write-Host "mesa: cached archive checksum MISMATCH (have $have) — re-downloading"
    Remove-Item -LiteralPath $archive -Force
  }
}

if ($needDownload) {
  Write-Host "mesa: downloading $url"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  # Invoke-WebRequest's progress renderer costs more than the transfer on a
  # 70 MB file in a non-interactive host.
  $oldPref = $ProgressPreference
  $ProgressPreference = "SilentlyContinue"
  try { Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing }
  finally { $ProgressPreference = $oldPref }
  $sw.Stop()
  $have = Get-Sha256 $archive
  if ($have -ne $Sha256.ToLowerInvariant()) {
    throw "mesa: SHA256 mismatch for $asset`n  expected $($Sha256.ToLowerInvariant())`n  actual   $have"
  }
  Write-Host ("mesa: downloaded + verified in {0:n1}s" -f $sw.Elapsed.TotalSeconds)
}

# ----------------------------------------------------------------- unpack ----
# Only the four files we actually use. The archive is solid, so a subset
# extraction still decompresses the block, but it keeps ~600 MB of unrelated
# payload (clon12compiler.dll, the WARP d3d10 driver, the media-foundation
# encoders, the whole x86 tree) off the disk and out of any cache.
$wanted = @(
  "x64\opengl32.dll",
  "x64\libgallium_wgl.dll",
  "x64\vulkan_lvp.dll",
  "x64\lvp_icd.x86_64.json"
)
$icd = Join-Path $x64 "lvp_icd.x86_64.json"

$allPresent = $true
foreach ($w in $wanted) {
  if (-not (Test-Path (Join-Path $unpack $w))) { $allPresent = $false }
}

if (-not $allPresent) {
  $sevenZip = $null
  $candidates = @("7z.exe", "7z")
  foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, "C:\Program Files")) {
    if ($root) { $candidates += (Join-Path $root "7-Zip\7z.exe") }
  }
  foreach ($cand in $candidates) {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if ($cmd) { $sevenZip = $cmd.Source; break }
  }
  if (-not $sevenZip) {
    throw "mesa: 7z.exe not found. mesa-dist-win ships .7z only; install 7-Zip (it is preinstalled on GitHub-hosted windows runners)."
  }
  Write-Host "mesa: extracting with $sevenZip"
  New-Item -ItemType Directory -Force -Path $unpack | Out-Null
  # Name-match both separator spellings. 7-Zip stores '/' in the archive but
  # matches '\' on Windows; an unmatched name is only a warning (exit 1), so
  # passing both is free insurance against a host-dependent surprise.
  $args7z = @("x", "-y", "-o$unpack", $archive) + $wanted + ($wanted | ForEach-Object { $_ -replace '\\', '/' })
  & $sevenZip @args7z | Out-Null
  # 7-Zip exit codes: 0 = OK, 1 = warning (e.g. a name matched nothing), >=2 =
  # fatal. Treat only >=2 as fatal here; the per-file assertion below is the
  # real gate, and it names the missing file instead of an opaque code.
  if ($LASTEXITCODE -ge 2) { throw "mesa: 7z extraction failed ($LASTEXITCODE)" }
} else {
  Write-Host "mesa: unpacked tree already present at $unpack"
}

foreach ($w in $wanted) {
  $p = Join-Path $unpack $w
  if (-not (Test-Path $p)) { throw "mesa: expected $w missing from $asset — has the archive layout changed?" }
}

# ------------------------------------------------------------------ stage ----
# Append rather than truncate: several StageTo targets may be provisioned by
# separate invocations, and -Unstage must be able to undo all of them.
$staged = New-Object System.Collections.Generic.List[string]
if (Test-Path $manifest) { Get-Content $manifest | Where-Object { $_.Trim() } | ForEach-Object { $staged.Add($_) } }

foreach ($dir in $StageTo) {
  if (-not $dir) { continue }
  if (-not (Test-Path $dir)) {
    Write-Host "mesa: stage target does not exist, skipping: $dir"
    continue
  }
  $full = (Resolve-Path $dir).Path
  foreach ($f in $glRuntime) {
    $dst = Join-Path $full $f
    Copy-Item -LiteralPath (Join-Path $x64 $f) -Destination $dst -Force
    if (-not $staged.Contains($dst)) { $staged.Add($dst) }
  }
  Write-Host "mesa: staged llvmpipe into $full"
}

if ($staged.Count) { Set-Content -LiteralPath $manifest -Value $staged -Encoding UTF8 }

# -------------------------------------------------------------------- env ----
# GALLIUM_DRIVER pins the gallium frontend to llvmpipe. libgallium_wgl.dll also
# carries the `d3d12` and `zink` drivers, and on this runner d3d12 would come up
# over the WARP adapter — a second, slower software path with a different bug
# surface. Naming the driver makes the arm reproducible instead of dependent on
# Mesa's probe order.
$envPairs = [ordered]@{
  "GALLIUM_DRIVER"                = "llvmpipe"
  "VK_DRIVER_FILES"               = $icd
  # Pre-1.3.207 spelling. Harmless on a newer loader (VK_DRIVER_FILES wins) and
  # keeps this working if the vcpkg loader pin ever moves backwards.
  "VK_ICD_FILENAMES"              = $icd
  "DXR_CTS_SOFTWARE_GFX"          = "1"
  "DXR_CTS_SOFTWARE_GFX_VERSION"  = "mesa-$Version (llvmpipe + lavapipe, mesa-dist-win)"
}

Write-Host ""
Write-Host "mesa: software graphics provisioned"
foreach ($k in $envPairs.Keys) { Write-Host "  $k=$($envPairs[$k])" }

# Which vulkan-1.dll will actually answer matters as much as which ICD is
# registered, and "the env var looked right" has already misled once here
# (#1525) — state the loader situation plainly.
Write-Host "mesa: Vulkan loader visibility"
$sys32Loader = Join-Path $env:SystemRoot "System32\vulkan-1.dll"
Write-Host "  System32\vulkan-1.dll present: $(Test-Path $sys32Loader)"
# The loader itself is staged by fetch_build_cts.bat as part of the CTS build
# (it belongs to the build, not to this arm — a per-run copy would be deleted
# again by -Unstage and break the cached tree). Just report what is there.
foreach ($dir in $StageTo) {
  if ($dir -and (Test-Path (Join-Path $dir "vulkan-1.dll"))) { Write-Host "  app-local loader present:     $dir\vulkan-1.dll" }
}

if ($GithubEnv -and $env:GITHUB_ENV) {
  foreach ($k in $envPairs.Keys) { "$k=$($envPairs[$k])" | Out-File -Append -Encoding utf8 $env:GITHUB_ENV }
  Write-Host "mesa: exported to `$GITHUB_ENV"
} else {
  foreach ($k in $envPairs.Keys) { Set-Item -Path "env:$k" -Value $envPairs[$k] }
}
