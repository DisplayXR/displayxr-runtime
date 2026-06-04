# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## Overview

DisplayXR is a lightweight standalone OpenXR runtime purpose-built for 3D displays (originally forked from **Monado**). It implements the Khronos OpenXR API on Windows, macOS, and Android. The runtime is vendor-agnostic — any 3D display vendor integrates via a plug-in DLL; **Leia SR** is the first integration.

**Current state:** native compositors ship for all major graphics APIs (D3D11, D3D12, Metal, OpenGL, Vulkan). The display extensions and the vendor plug-in extraction are complete. The spatial shell ships on Windows (macOS port deferred). Full status: [milestone tracker](https://github.com/DisplayXR/displayxr-runtime/milestones).

## Architecture

```
App (any graphics API)
        |
   OpenXR State Tracker
        |
   Core xrt interfaces
        |
   +----+-----+--------+--------+
   |    |     |        |        |
 D3D11 D3D12 Vulkan  Metal   OpenGL   <-- native compositors
   |    |     |        |        |
   Display Processor (vendor plug-in / sim_display)
        |
   Display
```

Each graphics API gets a native compositor — no interop, no Vulkan intermediary.

- Layer boundaries, the vendor-isolation rule, and "must NOT contain" constraints: `docs/architecture/separation-of-concerns.md`
- Why per-API compositors: `docs/adr/ADR-001-native-compositors-per-graphics-api.md`. Compositor never weaves (that's the DP's job): `docs/adr/ADR-007-compositor-never-weaves.md`
- Compositor pipeline: `docs/architecture/compositor-pipeline.md`
- Display processor vtable design (all 5 API variants): `docs/archive/vendor-integration-historical.md`

### Source tree (`src/xrt/`)
- **include/xrt/** — Core interface headers (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, …)
- **auxiliary/** — Shared utilities: math (`m_*`), util (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`)
- **compositor/** — Native compositors: `d3d11`, `d3d12`, `metal`, `gl`, `vk_native`, plus `multi`, `client`, `null` (headless, for testing). See `docs/architecture/project-structure.md`.
- **drivers/** — `sim_display/` (vendor-neutral simulation + plug-in), `qwerty/` (keyboard/mouse). Vendor plug-ins (Leia SR etc.) ship from their own repos per ADR-019.
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — IPC for service mode (preserve for `_ipc` apps, WebXR, multi-app shell)
- **targets/** — Build targets (runtime library, `displayxr-cli`, `displayxr-service`)

### Key interfaces (C, vtable-style polymorphism)
`xrt_device`, `xrt_compositor`, `xrt_instance`, `xrt_prober`. Full catalog incl. display-processor vtables: `docs/archive/vendor-integration-historical.md`.

### App classes
Four classes (handle, texture, hosted, IPC) — full reference: `docs/getting-started/app-classes.md`.
- `_handle` / `_texture` / `_hosted` → in-process `compositor/{d3d11,d3d12,metal,gl,vk_native}/`
- `_ipc` → `compositor/client/` → `ipc/` → `compositor/multi/` → native compositor (out-of-process)
- **Window handle:** `_handle` and `_texture` pass the app's **real** HWND (texture also passes a shared texture; its HWND is used for DP position tracking); `_hosted` passes **NULL** → the runtime self-creates a window at native res. The display processor always gets a real HWND — the app's, or the runtime's for hosted. (Branch: window-handling block in `*_compositor_create`.)
- **Texture-app layout:** a `_texture` app confines weaved 3D to a **canvas sub-rect** via `xrSetSharedTextureOutputRectEXT` (defaults to full client area; flows to the DP as `canvas_offset/size`) and may fill the **2D surround** outside it via `xrSetSharedTextureSurround2DEXT` (D3D11) / `xrSetSharedTextureSurround2DFenceEXT` (D3D12). Spec: `XR_EXT_win32_window_binding` §3.5–3.7.
- Test app naming: `cube_{class}_{api}_{platform}` (e.g. `cube_handle_metal_macos`, `cube_texture_d3d11_win`).

### Extension vs legacy apps
Full reference: `docs/architecture/extension-vs-legacy.md`. `_handle` and `_texture` are always extension apps; `_hosted` can be either. Legacy-app compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3-key mode selection for legacy apps (V toggle only).

### Key notes
- Compositor vtable has 56 methods — use the `comp_base` helper for boilerplate.
- **Two distinct swapchains** and the **canvas concept** (view dims + Kooima projection use canvas size, not display size): `docs/specs/runtime/swapchain-model.md`.

### Vendor plug-in integration
Vendor display drivers ship as **plug-in DLLs** from their own repos (ADR-019). `DisplayXR-LeiaSR.dll` (from `displayxr-leia-plugin`'s `src/drv_leia/`, entry point `xrtPluginNegotiate`) loads at `xrCreateInstance` via registry-driven discovery on Windows / JSON-manifest discovery on POSIX (`target_plugin_loader.c`). The runtime DLL has zero SR or `sim_display_*` identifiers in its link line (ADR-019 §2.1; CI-asserted in `build-windows.yml`). Eye tracking and display dimensions come through `iface->get_display_info` → `xrt_plugin_display_info` — no direct runtime → vendor call.
- Discovery / registration contract: `docs/specs/runtime/plugin-discovery.md`
- Vendor onboarding: `docs/guides/vendor-plugin-onboarding.md`

### Custom OpenXR extensions
- `XR_EXT_win32_window_binding` — app passes HWND to runtime
- `XR_EXT_cocoa_window_binding` — app passes NSWindow to runtime
- `XR_EXT_display_info` — display dimensions, eye-tracking modes
- `XR_EXT_android_surface_binding` — Android surface binding

Specs: `docs/specs/extensions/`. Eye-tracking MANAGED vs MANUAL contract: `docs/specs/vendor/eye-tracking-modes.md`.

## Build

Languages: C11 (core), C++17 (where needed), Python 3.6+ (build scripts).

### Windows (preferred on a Leia SR machine — iterate locally, not via CI)
```bat
scripts\build_windows.bat all        REM generate + runtime + installer + test apps
scripts\build_windows.bat build      REM runtime only (fastest iteration)
scripts\build_windows.bat installer  REM runtime installer only
scripts\build_windows.bat test-apps  REM test apps only (uses existing runtime build)
scripts\build_windows.bat generate   REM CMake generate only
```
First run downloads deps (vcpkg, OpenXR loader). Requires VS 2022 (C++ workload), Ninja, Vulkan SDK, GitHub CLI. Outputs to `_package/` (runtime) and `test_apps/*/build/`. Always build through this script — never call cmake/ninja directly on Windows.

### macOS
```bash
# brew install cmake ninja eigen vulkan-sdk
./scripts/build_macos.sh
```
Builds runtime, OpenXR loader, test apps. The macOS Vulkan native compositor runs via MoltenVK over a CAMetalLayer-backed surface (`cube_handle_vk_macos`); an earlier `VK_ERROR_EXTENSION_NOT_PRESENT` failure was a MoltenVK-era issue since resolved. The one dev gotcha is the two-`libvulkan` loader-image conflict (dev build vs installed runtime) — see `docs/getting-started/building.md` and pin `XR_RUNTIME_JSON` / share one loader image.

### Standard CMake
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja && cmake --build .
```
Key options: `XRT_FEATURE_SERVICE` (out-of-process service mode), `BUILD_TESTING`. Vendor DP code is **not** a CMake option — all `drv_*` implementations ship as plug-in DLLs discovered at `xrCreateInstance`.

### Tests & formatting
```bash
cd build && ctest
git clang-format          # format only your changes (preferred)
scripts/format-project.sh # format all
```

### Windows compile-check on macOS / Linux (MinGW-w64)
```bash
brew install mingw-w64                       # one-time
./scripts/build-mingw-check.sh               # default: aux_util displayxr_mcp
./scripts/build-mingw-check.sh aux_util drv_qwerty
```
Cross-compiles a curated subset to catch Win32 typos, missing `#ifdef XRT_OS_WINDOWS` guards, and wrong-platform symbols **before CI**. Toolchain `cmake/toolchain-mingw-w64.cmake`; output `build-mingw/` (gitignored); ~30 s after first configure.

**MinGW is NOT a full MSVC substitute** — it can't cross-compile WIL (`wil::com_ptr` in `comp_d3d11_service.cpp`) or Vulkan/vcpkg-only targets, and winpthreads adds POSIX extensions MSVC lacks. To stay portable:
- Use `os_monotonic_get_ns()` (not `clock_gettime(CLOCK_MONOTONIC,…)`); C11 `timespec_get(TIME_UTC)` (not `clock_gettime(CLOCK_REALTIME,…)`); `os_nanosleep()` (not `usleep()`).
- Use `long` not `pid_t` in public headers; avoid `<unistd.h>`/`<sys/types.h>` in cross-platform code. For PIDs, wrap (`getpid()` / `GetCurrentProcessId()`).
- Avoid C11 `<stdatomic.h>` — use a `pthread_mutex_t` or `Interlocked*` behind `#ifdef`.
- `strncasecmp`/`strcasecmp` are POSIX — add `#ifdef _WIN32 #define strncasecmp _strnicmp #endif`.

### CI
Public-repo CI is free, so `build-windows.yml` + `build-macos.yml` fire on every PR (drafts included), every push to `main`, and every `v*` tag. Guards:
- **Doc-only short-circuit** — `DetectChanges.outputs.docs_only` lets `Runtime`/`Build`/`shell-path-guard` report success in ~30 s without building, so they can be required status checks without blocking doc PRs.
- **Cancel-in-progress** — rapid pushes cancel earlier runs.
- **`shell-path-guard`** — fails any push reintroducing `src/xrt/targets/shell/*` or `installer/DisplayXRShellInstaller.nsi` (those live in `displayxr-shell-pvt`).
- **Headless self-test gate** — the `Runtime` job registers the freshly-built sim-display plug-in in `HKLM\Software\DisplayXR\DisplayProcessors` and runs `displayxr-cli selftest` (see below). This is the only CI step that *executes* the runtime (every other job just builds); it gates on plug-in discovery + ABI + display-info validity, hardware-free.
- **No per-CI OneDrive upload** — `build-windows.yml` no longer rclone-copies build artifacts (runtime installer / test-apps zip) to OneDrive. The OneDrive copy now happens **on bundle release only**, in `displayxr-installer`'s `publish-bundle.yml` (uploads `DisplayXRBundle-*.exe` to `SANDBOX/RUNTIMES + SDK + PLUGINS/OpenXR`, gated on its own `RCLONE_CONFIG` secret). CI artifacts stay on GitHub via `upload-artifact`.

For tagged releases use `/release` — don't tag manually.

## Releasing

This repo IS the public runtime (no private→public mirror). A release is a `vX.Y.Z` tag → parallel Windows + macOS CI → GitHub Release with both installers (`DisplayXRSetup-*.exe`, `DisplayXR-Installer-*.pkg`) attached. (Test apps aren't packaged — CI compiles them as a check on `test_apps/` changes only, no artifacts.)

```
/release v1.2.1   # explicit            /release patch
/release minor                          /release major
```
`/release` tags `main` HEAD (CI patches `CMakeLists.txt VERSION` from `git describe` at build time, so the source-tree value isn't committed), watches both CI workflows, writes curated notes, then waits for `BumpVersionsJsonOnTag` (ABI gate + `versions.json` bump + mirror to `displayxr-installer`). macOS `.pkg` is a soft requirement (warns, doesn't block). Auto-bump regex is strict (`^v[0-9]+\.[0-9]+\.[0-9]+$`) so non-canonical tags are skipped. See `.claude/skills/release/SKILL.md`.

**Sibling release flows (NOT `/release` in this repo):**

| Component | Repo | How |
|---|---|---|
| Shell | `displayxr-shell-pvt` → `displayxr-shell-releases` | `/dxr-release` or `git tag` → `publish-shell-releases.yml` builds + cross-publishes + dispatches `versions-bump`. Auth via `displayxr-publish-bot` GitHub App (`.secrets/displayxr-publish-bot.pem` backup; see `.secrets/NOTE.md`). |
| Leia SR plug-in | `displayxr-leia-plugin` | `/dxr-release` → builds DLL + installer + dispatches `versions-bump` with ABI gate (ADR-020). |
| MCP framework | `displayxr-mcp` | `/dxr-release` → matrix build + dual-platform installers (`DisplayXRMCPSetup-*.exe` NSIS + `DisplayXRMCP-*.pkg` productbuild) + dispatches `versions-bump`. |
| Extension headers | `displayxr-extensions` | Auto-syncs from `src/external/openxr_includes/` on every push to main. No tag. |
| Standalone demos | `displayxr-demo-*` | `/dxr-release` → builds installer + dispatches `versions-bump`. |
| Meta-installer bundle | `displayxr-installer` | `/installer-release` or `workflow_dispatch` (NOT auto-fired). Chains every component installer. On release, uploads `DisplayXRBundle-*.exe` to OneDrive (the sole OneDrive upload point; runtime CI no longer does). |

`/dxr-release` handles every sibling repo; `/installer-release` handles the bundle. Neither applies to this repo (use `/release` here).

**Release skills are hub-homed and symlinked — one source of truth, globally invocable.** `dxr-release` + `installer-release` live git-tracked **only** in this repo at `.claude/skills/` (alongside `/release`). They are NOT copied into the sibling repos. Both are *parameterized*, not cwd-detecting, so they're driven from this runtime hub:
- `/dxr-release <component> <version>` — e.g. `/dxr-release mcp v0.3.4`. Resolves component → repo, clones it to a temp dir, tags, watches CI + the dispatched `versions-bump.yml`, reports. Components: `shell`, `leia-plugin`, `mcp`, `gauss`.
- `/installer-release <version>` — fires `publish-bundle.yml` via `gh workflow run` against `displayxr-installer`; no local checkout needed.
- `/sync-website [--dry-run] [focus]` — **editorial** sync pass for `displayxr-website`. Mechanical facts (versions, demo cards, repo list, extension names) auto-sync via the website's `sync-org.yml` (direct-commit to its `main`); this skill handles the class-B *narrative* — surfaces new ADRs / repos / extensions / closed milestones and authors the matching roadmap/architecture/ecosystem prose into the site's hand-written TSX, opening a PR. Design: `displayxr-website/docs/org-sync.md`.

To make them invocable from *any* directory (not just a runtime checkout), `scripts/link-dxr-skills.sh` symlinks `.claude/skills/{dxr-release,installer-release,sync-website}` into `~/.claude/skills/`. **`setup-displayxr.{sh,bat}` runs this automatically**, so a fresh dev box gets it for free; on an existing box, run `./scripts/link-dxr-skills.sh` once (`--check` to inspect, `--unlink` to undo). The symlink means the bytes stay git-tracked here while being reachable globally — edit the canonical copy in this repo, never the symlink target. Windows: the `.bat` orchestrator uses `mklink /D` (needs the elevated terminal it already requires, or Developer Mode).

### versions.json — single source of truth
`versions.json` at the repo root is the canonical pin matrix consumed by `scripts/setup-displayxr.{sh,bat}` and mirrored byte-for-byte by `displayxr-installer`. **Auto-bumped on every component release** (the dispatch flow above updates the matching field on `main` and mirrors to `displayxr-installer/main` within ~30 s via the publish bot). Two safety nets:
- **ABI gate** (`scripts/check_plugin_abi.py`) — on any `leia_plugin` bump and every runtime self-bump; if the plug-in's reported ABI ≠ runtime's `XRT_PLUGIN_API_VERSION_CURRENT`, the bump is skipped and a tracking issue auto-opens on `displayxr-leia-plugin`.
- **Drift guard** in `displayxr-installer`'s `publish-bundle.yml` — `assert-versions-in-sync` diffs the mirror against `runtime/main` before any bundle build.

Full spec: `docs/specs/runtime/versions-json-autobump.md`.

## Repos & issue routing

| Repo | Visibility | Contents |
|---|---|---|
| `displayxr-runtime` | Public | Runtime source + dev issues + CI. **This repo.** |
| `displayxr-runtime-legacy-mirror` | Public (archived) | Pre-deprivatize history; read-only. Old links auto-redirect. |
| `displayxr-shell-pvt` | Private | Shell source, dev issues, CI. |
| `displayxr-shell-releases` | Public | Shell installer releases (auto-published on tags) + user-facing bug reports. |
| `displayxr-leia-plugin` | Public | Leia SR DP plug-in source + `DisplayXRLeiaSRSetup-*.exe`. |
| `displayxr-extensions` | Public | OpenXR extension headers, auto-synced from this repo. |
| `displayxr-demo-*` | Public | Standalone demos (independent evolution; no source-mirror). |

**Issue rules:** runtime dev issues → this repo; shell dev issues → `displayxr-shell-pvt`; user-facing shell bugs → `displayxr-shell-releases` (triage → file dev issue on shell-pvt if actionable). Never dual-create across repos. External contributors PR directly against this repo.

## Running & testing locally

### Running without installing
```bash
# Linux / macOS
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```
```cmd
:: Windows — non-elevated terminal (see caveat)
_package\run_cube_handle_d3d11_win.bat
```
**Elevated-terminal caveat:** the bundled Khronos `openxr_loader.dll` (1.1.38 and 1.1.43) silently ignores `XR_RUNTIME_JSON` in elevated/admin processes and falls back to `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`. Use a non-elevated terminal, or push the rebuilt DLL to Program Files.

**Which DLL loaded?** Every `xrCreateInstance` logs a WARN line near the top of `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log` — search `loaded from:` for the authoritative path. Full dev-iteration reference: `docs/getting-started/building.md`.

After a rebuild, copy runtime binaries into `C:\Program Files\DisplayXR\Runtime` (registry discovery finds it); only run the installer when the installer itself changed.

### Headless diagnostics (`displayxr-cli`)
`displayxr-cli` runs the runtime **without a compositor/GPU/window** — it exercises the real plug-in discovery + display-processor path in-process (`target_instance_no_comp`), so it's the fastest way to check "did the runtime start, find a DP, and get sane display info?" without launching an app.
- `displayxr-cli selftest` — asserts a DP-backed head device exists, a vendor plug-in is active (the loader rejects ABI-mismatched plug-ins, so this *is* an ABI check), and display dims are valid. Strict exit code; this is what the CI gate runs.
- `displayxr-cli info` — bug-report dump: runtime version/git-tag, plug-in ABI, active plug-in identity + display info (dims, viewer, eye-tracking modes), and the Windows `ActiveRuntime` value.

Both use registry discovery on Windows (so they pick up whatever plug-in is installed — Leia SR or sim-display), `XRT_PLUGIN_SEARCH_PATH` on POSIX.

### Windows test apps
`scripts\build_windows.bat test-apps` builds apps and generates run scripts in `_package/` that set `XR_RUNTIME_JSON` to the dev build.

| App | Run script |
|---|---|
| cube_handle_d3d11_win | `_package\run_cube_handle_d3d11_win.bat` |
| cube_hosted_d3d11_win | `_package\run_cube_hosted_d3d11_win.bat` |
| cube_handle_d3d12_win | `_package\run_cube_handle_d3d12_win.bat` |
| cube_handle_gl_win | `_package\run_cube_handle_gl_win.bat` |
| cube_handle_vk_win | `_package\run_cube_handle_vk_win.bat` |

**Forcing the IPC/service path (easiest way to exercise IPC without the shell).**
By default `handle`/`hosted` apps run **in-process** (local compositor, app- or
runtime-owned window) — a running `displayxr-service.exe` does *not* by itself
make an app an IPC client. To put **any** existing app on the IPC path, start the
service and launch the app with `XRT_FORCE_MODE=ipc` (read by the runtime DLL;
this is exactly what `webxr_bridge` sets via `force_ipc_mode_env()`):
```cmd
_package\bin\displayxr-service.exe                            REM D3D11 service compositor
set XRT_FORCE_MODE=ipc && test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```
The app then creates a `client_d3d11_compositor` (`server-creates-swapchain`
model), `is_service_mode` flips true, and `is_*_native_compositor` stays false —
so e.g. `xrCaptureAtlasEXT` routes through the IPC branch, not the in-process
one. (`XRT_FORCE_MODE=ipc` must be set process-level via the env, not the run
script, since the DLL has its own static-CRT environment block.)

### macOS test apps
Copy binaries to `_package/DisplayXR-macOS/bin/`. Generated `run_*.sh` set `XRT_PLUGIN_SEARCH_PATH=$DIR/lib/displayxr/plugins` so the dev tree's `DisplayXR-SimDisplay.dylib` (+ `200-sim-display.json`) is discovered without touching `~/Library/Application Support/`.

| App | Run script |
|---|---|
| cube_handle_vk_macos | `run_cube_handle_vk.sh` |
| cube_handle_metal_macos | `run_cube_handle_metal.sh` |
| cube_handle_gl_macos | `run_cube_handle_gl.sh` |
| cube_texture_metal_macos | `run_cube_texture_metal.sh` |
| cube_hosted_metal_macos | `run_cube_hosted_metal.sh` |
| cube_hosted_legacy_metal_macos | `run_cube_hosted_legacy_metal.sh` |

### Shell mode (Windows)
The DisplayXR Shell ships from `displayxr-shell-pvt` via `DisplayXRShellSetup-*.exe`, installs into the runtime tree (`C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe`), requires the runtime as a hard prereq (`HKLM\Software\DisplayXR\Runtime\InstallPath`), and registers at `HKLM\Software\DisplayXR\WorkspaceControllers\shell`. **This repo does NOT build a shell binary** — any `_package\bin\displayxr-shell.exe` is stale; ignore/delete. The runtime owns no specific workspace app; third-party verticals follow the same registration contract (`docs/specs/runtime/workspace-controller-registration.md`).

`displayxr-shell.exe` auto-starts the service, activates shell mode via IPC, sets `XR_RUNTIME_JSON` + `DISPLAYXR_WORKSPACE_SESSION=1`, launches apps, and monitors clients:
```
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" app1.exe app2.exe
```
Optional per-app pose (`--pose x,y,z,width_m,height_m` before each app path):
```
"...\displayxr-shell.exe" --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```
**Launching from Claude Code:** use `displayxr-shell.exe` with `run_in_background: true` and `timeout: 600000` (see the shell-mode launch section above + `_package\run_shell_*.bat`).

**Shell controls:** left-click=focus, title-bar drag=move, edge drag=resize, right-click=focus+forward to app, double-click title bar=maximize/restore, scroll=resize, Ctrl+1-4=layout presets, TAB=cycle focus, DELETE=close app, ESC=dismiss, V=toggle 2D/3D, WASD/left-drag=app input. Title-bar buttons: close (X), minimize (—).

## Autonomous capture & debugging

### Windows compositor screenshot (preferred)
The D3D11 service compositor file-triggers a full-resolution atlas capture (reads the D3D11 texture directly — no DPI/PrintWindow issues). Code: `comp_d3d11_service.cpp`, in `multi_compositor_render()` before `Present()`.
```bash
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot.png"
touch "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot_trigger"
sleep 3   # then Read C:\Users\SPARKS~1\AppData\Local\Temp\workspace_screenshot.png
```
%TEMP% screenshot artifacts are pre-authorized for read/write. Screenshots taken during eye-tracking warmup may miss UI — when correctness depends on visuals, ask the user to eyeball the live display.

**Toggle the shell launcher (Ctrl+L) programmatically** via PostMessage to the message-only window:
```powershell
Add-Type @'
using System;using System.Runtime.InteropServices;
public class ShellMsg{
[DllImport("user32.dll",CharSet=CharSet.Ansi)] public static extern IntPtr FindWindowExA(IntPtr p,IntPtr a,string c,string t);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
$h=[ShellMsg]::FindWindowExA([IntPtr]::new(-3),[IntPtr]::Zero,'Static','DisplayXR Shell Msg')
[ShellMsg]::PostMessage($h,0x0312,[IntPtr]::new(2),[IntPtr]::Zero)
```

### macOS test-app pixels
`screencapture`/`CGWindowListCreateImage`/`osascript keystroke` all need TCC permissions the harness lacks. Instead dump from inside the app via `stbi_write_png` (forward-declare `extern "C" int stbi_write_png(...)` at file scope of the `.mm`, dump on a frame-counter gate ~120 — HUD strings are throttled to ~2 Hz so frame-0 dumps are blank). Preferred: the metal + GL compositors poll `/tmp/dxr_atlas_trigger` and write the composited content region (post per-tile, pre DP) to `/tmp/dxr_atlas.png`:
```bash
rm -f /tmp/dxr_atlas.png /tmp/dxr_atlas_trigger
_package/DisplayXR-macOS/run_cube_handle_metal.sh > /tmp/log 2>&1 &
sleep 4 && touch /tmp/dxr_atlas_trigger && sleep 2
pkill -f cube_handle_metal_macos   # then Read /tmp/dxr_atlas.png
```
(vk_native has no trigger yet — use the in-app `stbi_write_png` trick.)

### Crash debugging (procdump + cdb)
```bash
# Capture (download procdump64.exe from https://live.sysinternals.com/procdump64.exe)
procdump64.exe -accepteula -e -ma -x . path/to/app.exe
# Analyze (cdb ships with WinDbg)
CDB="/c/Program Files/WindowsApps/Microsoft.WinDbg_.../amd64/cdb.exe"
"$CDB" -z crash.dmp -c ".ecxr; kP 15; q"        # stack
"$CDB" -z crash.dmp -c ".ecxr; ub ADDR L15; q"  # disasm backwards from return addr
"$CDB" -z crash.dmp -c ".ecxr; dqs ADDR L20; q" # dump memory / vtable
```
Release builds lack PDBs — use `ub` to find the calling pattern (`mov rax,[rbx+offset]; call rax` = vtable call) and cross-reference the offset with struct defs. Common patterns: `call rax` with `rax=0` → null fn pointer in a vtable/dispatch table; VK dispatch nulls → app's device missing extension fns (use `submit_fallback`); `ACCESS_VIOLATION writing 0x0` at `0x0` → call through null fn pointer; crash in `DisplayXRClient.dll` without symbols → use `DisplayXRClient+OFFSET`.

## Conventions

### Debug logs (`docs/reference/debug-logging.md`)
- `U_LOG_W` (WARN) only for one-off init/error/lifecycle events.
- `U_LOG_I` (INFO) for recurring/throttled diagnostics — but aux INFO is dropped from the hot path, so its absence ≠ "didn't run".
- **Never** add per-frame `U_LOG_W` — massive log bloat.

### Other skills
- `/ask-gemini` — read-only code analysis report via Gemini (`~/.claude/skills/ask-gemini/SKILL.md`).
- `/new-displayxr-app <name> [class=] [api=] [platform=]` — scaffold a new app correct-by-construction (clones the nearest `cube_*` reference, drops manifest + per-app CLAUDE.md, wires CMake, lints). Backed by `docs/guides/displayxr-app-rules.md`.

### Writing / checking a DisplayXR app
The authoring invariants live in `docs/guides/displayxr-app-rules.md` (INV-* rules for views, swapchain tiling, color, capture, manifest/logos, workspace + the `F-*` OpenXR foundations). Lint any app against them with `python3 scripts/check_displayxr_app.py <app-dir>` (`--list-rules` for the catalog) — run it before calling an app done.

## Documentation index

See `docs/README.md` for the full index. By task:

| When you need to… | Read |
|---|---|
| Layer boundaries (what goes where) | `docs/architecture/separation-of-concerns.md` |
| Build a workspace app (shell-style controller) | `docs/specs/runtime/workspace-controller-registration.md` |
| Add a new display vendor | `docs/guides/vendor-plugin-onboarding.md` → `docs/reference/xrt_plugin_iface.md` + `docs/specs/runtime/plugin-discovery.md` |
| `xrt_plugin_iface` callbacks | `docs/reference/xrt_plugin_iface.md` |
| Multiview tiling / atlas layout | `docs/specs/runtime/multiview-tiling.md` |
| Extension API (display_info, window bindings) | `docs/specs/extensions/XR_EXT_display_info.md` |
| Why an architectural decision was made | `docs/adr/` |
| Write / scaffold / lint a DisplayXR app | `docs/guides/displayxr-app-rules.md` (+ `/new-displayxr-app`, `scripts/check_displayxr_app.py`) |
| Legacy vs extension app differences | `docs/architecture/extension-vs-legacy.md` |
| Eye-tracking MANAGED/MANUAL contract | `docs/specs/vendor/eye-tracking-modes.md` |
| Add a new OpenXR extension | `docs/guides/implementing-extension.md` |
| Write a device driver | `docs/guides/writing-driver.md` |
| Leia SR weaver internals (DX11/DX12/GL/VK) | `docs/vendors/leia/weaver.md` |
| Leia transparency — **primary path**: WGC background-capture (compose-under-bg) on D3D11/D3D12/VK | `docs/vendors/leia/transparency.md` |
| Leia chroma-key overlay — **legacy fallback, not the expected path** (background capture is; chroma-key survives only on the GL DP) | `docs/vendors/leia/chroma-key-overlay.md` |
| Leia window phase-snapping (WndProc snap + resolved WndProcDispatcher race) | `docs/vendors/leia/window-phase-snapping.md` |
| Leia display mode switching (2D/3D: SwitchableLensHint / backlight) | `docs/vendors/leia/display-mode-switching.md` |
| Kooima projection math | `docs/architecture/kooima-projection.md` |
| Compositor pipeline | `docs/architecture/compositor-pipeline.md` |
| Swapchain model / canvas | `docs/specs/runtime/swapchain-model.md` |
| Workspace ↔ runtime contract / boundary | `docs/architecture/separation-of-concerns.md`, `docs/roadmap/workspace-runtime-contract.md` |
| Vendor-specific docs | `docs/vendors/<vendor>/README.md` |
| 3D capture pipeline | `docs/roadmap/3d-capture.md` |
| Workspace/runtime IPC contract | `docs/roadmap/workspace-runtime-contract.md` |
| Product vision | `docs/roadmap/spatial-desktop-prd.md` |
