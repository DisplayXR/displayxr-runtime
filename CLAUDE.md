# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

DisplayXR is a lightweight standalone OpenXR runtime purpose-built for 3D displays. The foundation work (issue #23) is complete — stripped from 500+ files to ~150, with native compositors for every major graphics API.

### Milestone Progress

See the [milestone tracker](https://github.com/DisplayXR/displayxr-runtime-pvt/milestones) for full status.

- **M1: Foundation** — Done. Stripped 34 VR drivers, removed Vulkan server compositor, cleaned CMake, extracted stereo math.
- **M2: Native Compositors** — Done. D3D11, D3D12, Metal, OpenGL, Vulkan all shipping.
- **M3: Test Coverage** — #30, #31, #33 open.
- **M4: Display Extensions** — Done. `XR_EXT_display_info` header frozen at v12 (#114 closed). Events (#3), multiview math (#38), eye tracking modes (#81), docs (#66) all complete. Vendor-initiated transition detection (#123) shipped via `get_hardware_3d_state()` DP vtable method. Remaining vendor work (MANUAL eye tracking mode) blocked on Leia SDK.
- **M5: Interface Standardization** — #45, #46, #47 open.
- **M6: Spatial Shell** — Done (Windows). Phases 0–8 shipped: multi-compositor, spatial windowing, window chrome, layout presets, 2D app capture, focus-adaptive 2D/3D mode, app launcher, graceful exit, 3D capture (Ctrl+Shift+C). macOS port deferred.
- **MCP (AI-Native Control)** — Phase A (handle-app introspection) and Phase B (service-mode shell tools) merged. Tools: `get_kooima_params`, `capture_frame`, `list_windows`, `get/set_window_pose`, `save/load_workspace`, `apply_layout_preset`. Spec: `docs/roadmap/mcp-spec-v0.2.md`.

### Architecture

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
   Display Processor (LeiaSR / sim_display)
        |
   Display
```

Each graphics API gets a native compositor — no interop, no Vulkan intermediary.

For layer boundaries and what each layer owns, see `docs/architecture/separation-of-concerns.md`.
Why each API gets its own compositor: `docs/adr/ADR-001-native-compositors-per-graphics-api.md`.

### Four App Classes

See `docs/getting-started/app-classes.md` for the full reference (handle, texture, hosted, IPC).

**Key code paths by class:**
- `_handle` / `_texture` / `_hosted` → `compositor/{d3d11,d3d12,metal,gl,vk_native}/` (in-process)
- `_ipc` → `compositor/client/` → `ipc/` → `compositor/multi/` → native compositor (out-of-process)

Test app naming: `cube_{class}_{api}_{platform}` — e.g. `cube_handle_metal_macos`, `cube_texture_d3d11_win`, `cube_hosted_d3d11_win`.

### Extension Apps vs Legacy Apps

See `docs/architecture/extension-vs-legacy.md` for the full reference.

Key facts for AI context: `_handle` and `_texture` are always extension apps. `_hosted` can be either. Legacy app compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

### Key Architectural Notes
- Compositor vtable has 56 methods — use `comp_base` helper for boilerplate
- IPC/service mode (`ipc/`, `compositor/client/`, `compositor/multi/`) must be preserved for `_ipc` apps, WebXR, and multi-app spatial shell
- `compositor/null/` — headless compositor for testing
- **Two distinct swapchains** — see `docs/specs/swapchain-model.md`
- **Canvas concept** — view dims and Kooima projection use canvas size, not display size. See `docs/specs/swapchain-model.md`.
- **Compositor pipeline** — see `docs/architecture/compositor-pipeline.md`

For the vendor isolation rule and layer "must NOT contain" constraints, see `docs/architecture/separation-of-concerns.md`.
For display processor vtable design (all 5 API variants), see `docs/guides/vendor-integration.md`.

## Project Overview

DisplayXR is a lightweight OpenXR runtime purpose-built for 3D displays (originally forked from **Monado**). It implements the OpenXR API standard from Khronos and runs on Windows, macOS, and Android. The runtime is vendor-agnostic — any 3D display vendor can integrate their driver. **Leia SR SDK** is the first vendor integration.

## Build Commands

### Local macOS Build
```bash
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
./scripts/build_macos.sh
```
Builds the runtime, OpenXR loader, and test apps. The Vulkan compositor will fail at runtime with `VK_ERROR_EXTENSION_NOT_PRESENT` (MoltenVK limitation, not a build issue).

### Local Windows Build
```bat
scripts\build_windows.bat all        REM Full build (generate + runtime + installer + test apps)
scripts\build_windows.bat build      REM Runtime only (fastest iteration)
scripts\build_windows.bat test-apps  REM Test apps only (uses existing runtime build)
scripts\build_windows.bat generate   REM CMake generate only
```
Downloads all dependencies on first run (SR SDK, vcpkg, OpenXR loader). Requires VS 2022 with C++ workload, Ninja, Vulkan SDK, and GitHub CLI. Outputs to `_package/` (runtime) and `test_apps/*/build/` (test apps).

**When on a Windows machine with a Leia SR display, prefer local builds over CI** — iterate faster with `scripts\build_windows.bat build` and test directly. Run scripts are generated in `_package/` (see Windows Test App section below).

### Windows Compile-Check on macOS / Linux (MinGW-w64)
```bash
brew install mingw-w64        # one-time
./scripts/build-mingw-check.sh                # default targets: aux_util mcp_adapter
./scripts/build-mingw-check.sh aux_util drv_qwerty  # custom target list
```
Cross-compiles a curated subset against MinGW-w64 to catch Win32-API typos, missing `#ifdef XRT_OS_WINDOWS` guards, and wrong-platform symbols **before pushing to CI**. Mirrors the displayxr-unity plugin's `native~/build-win.sh` pattern.

**Caveats — MinGW is NOT a full MSVC substitute:**
- Compositors using WIL (`wil::com_ptr` in `comp_d3d11_service.cpp`) won't cross-compile. Service-side D3D11 stays MSVC-only.
- Vulkan / vcpkg-only deps not available; targets requiring them (full openxr_displayxr.dll, comp_d3d11 native) are out of scope.
- **MinGW ships winpthreads which adds POSIX-like extensions** (`clock_gettime`, `CLOCK_MONOTONIC`, `pid_t`, `<unistd.h>`, etc.) that **MSVC does not have**. These bugs only surface in real CI. Workarounds:
  - Use `os_monotonic_get_ns()` from `aux/os/os_time.h` instead of `clock_gettime(CLOCK_MONOTONIC, …)`.
  - Use C11 `timespec_get(TIME_UTC)` instead of `clock_gettime(CLOCK_REALTIME, …)`.
  - Use `long` instead of `pid_t` in public headers (don't include `<sys/types.h>` for it).
  - Avoid `<unistd.h>` in cross-platform code. Use `os_nanosleep()` from `aux/os/os_time.h` instead of `usleep()`. For PIDs, use a wrapper helper like `u_mcp_self_pid()` that does `getpid()` on POSIX and `GetCurrentProcessId()` on Windows.
  - Avoid C11 `<stdatomic.h>`. MSVC needs `/experimental:c11atomics` and even then `_Atomic(T*)` syntax is unreliable. Use a `pthread_mutex_t` (uncontended is essentially free), or Windows `Interlocked*` APIs behind an `#ifdef`.
  - `strncasecmp` / `strcasecmp` are POSIX. MSVC has `_strnicmp` / `_stricmp` in `<string.h>`. Add `#ifdef _WIN32 #define strncasecmp _strnicmp #endif` at the top of the TU.

What it DOES catch reliably: `windows.h` symbol resolution, missing platform-config includes (`xrt/xrt_config_os.h` so `XRT_OS_WINDOWS` is actually defined), duplicate struct definitions across `#ifdef` branches, mistakes in cross-platform pthread wrapping.

Toolchain: `cmake/toolchain-mingw-w64.cmake`. Output goes to `build-mingw/` (gitignored). Runs in ~30 s after first configure.

### CI Build (Remote)
```bash
/ci-monitor "your commit message"
```
Commits, pushes, monitors GitHub Actions (Windows + macOS), auto-fixes common build errors. Use for feature branch (`**-ci`) validation when remote CI is needed before merge. **Not needed for every push** — prefer local builds for daily development.

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### With LeiaSR SDK Support
Set `LEIASR_SDKROOT` environment variable. Found via `find_package(simulatedreality CONFIG)` and `find_package(srDirectX CONFIG)`.

### Running Tests
```bash
cd build && ctest
```

### Code Formatting
```bash
git clang-format    # Format only your changes (preferred)
scripts/format-project.sh   # Format all
```

### Publishing to Public Repos (Automated)

On tagged releases (`v*`), the `publish-public.yml` workflow automatically:
1. Pushes runtime code (with shell stripped) to `DisplayXR/displayxr-runtime` (public)
2. Publishes combined installer + shell binary to `DisplayXR/displayxr-shell-releases`
3. Creates GitHub Releases on both public repos

```bash
# Preferred: use the /release skill (handles version bump, tagging, monitoring, verification)
/release v1.0.0          # explicit version
/release patch           # auto-bump patch

# Manual alternative:
git tag v1.x.x
git push origin v1.x.x    # triggers CI + auto-publish to public repos
```

Extension headers auto-publish to `DisplayXR/displayxr-extensions` via `publish-extensions.yml` on every push to main.

Demos publish **one repo per demo** on each `v*` tag — see `docs/roadmap/demo-distribution.md` and `docs/guides/add-new-demo-repo.md`. Each demo gets its own `publish-demo-<name>.yml` workflow that syncs source + attaches a binary Release zip. Currently the only per-demo repo is `DisplayXR/displayxr-demo-gaussiansplat`; `demos/spatial_os_handle_d3d11_win/` stays in this repo as a reference implementation (superseded by the Shell feature) and does not publish.

### Repository Structure

| Repo | Visibility | Contents |
|------|-----------|----------|
| `DisplayXR/displayxr-runtime-pvt` | **Private** (dev) | Runtime + shell source, all dev issues, CI builds. **This repo.** |
| `DisplayXR/displayxr-runtime` | Public | Public releases — auto-published from `-pvt` on tags, shell code stripped |
| `DisplayXR/displayxr-shell-releases` | Public | Binary-only shell releases, user-facing bug reports |
| `DisplayXR/displayxr-extensions` | Public | OpenXR extension headers, auto-synced from `src/external/openxr_includes/` |
| `DisplayXR/displayxr-demo-<name>` | Public | One repo per standalone demo (source + binary releases). Currently `displayxr-demo-gaussiansplat`. |

Shell code lives in `src/xrt/targets/shell/`. It is developed alongside the runtime in this repo. The shell is proprietary; it is excluded from public releases by the publish workflow.

### Issue Management

**All dev issues go to `DisplayXR/displayxr-runtime-pvt`** — both runtime and shell. Use the `shell` label to distinguish shell-specific issues.

| Where | What | Who |
|-------|------|-----|
| `DisplayXR/displayxr-runtime-pvt` | All dev issues (bugs, tasks, implementation) | Developers |
| `DisplayXR/displayxr-runtime` | Curated public milestones only (~5-10 issues) | Public / OEMs |
| `DisplayXR/displayxr-shell-releases` | User-facing shell bug reports | Shell users |

**Rules:**
- Never dual-create issues across repos. One source of truth per issue.
- Create dev issues on `DisplayXR/displayxr-runtime-pvt` only.
- Update public milestone issues on `DisplayXR/displayxr-runtime` at major milestones, don't create new ones for subtasks.
- If a user files a bug on `displayxr-shell-releases`, triage it and create a dev issue on the private repo if actionable.
- Community contributions: external contributors submit PRs to `DisplayXR/displayxr-runtime` (public). Accepted PRs are applied to `-pvt` via `scripts/apply-public-pr.sh`.

## Architecture

### Source Tree Structure (`src/xrt/`)
- **include/xrt/** — Core interface headers (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, etc.)
- **auxiliary/** — Shared utilities: math (`m_*`), utilities (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`)
- **compositor/** — Native compositors (D3D11, D3D12, Metal, GL, Vulkan, multi, client, null). See `docs/architecture/project-structure.md`.
- **drivers/** — `leia/` (LeiaSR SDK), `sim_display/` (simulation), `qwerty/` (keyboard/mouse controllers)
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service, displayxr-shell)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

For the full interface catalog including display processor vtables (5 API variants), see `docs/guides/vendor-integration.md`.

### LeiaSR SDK Integration
- `XRT_HAVE_LEIA_SR` CMake option (auto-enabled if SDK found)
- D3D11 weaver: `compositor/d3d11/` via `drivers/leia/leiasr_d3d11.cpp`
- Eye tracking via SR SDK's LookaroundFilter
- Display dimensions from SR::Display for Kooima asymmetric frustum projection

### Native Compositors
Each bypasses Vulkan entirely for its graphics API:
- **D3D11** (`compositor/d3d11/`) — Shipping. LeiaSR D3D11 weaver, `XR_EXT_win32_window_binding`
- **D3D12** (`compositor/d3d12/`) — Shipping. `XR_EXT_win32_window_binding`
- **Metal** (`compositor/metal/`) — Shipping. sim_display weaver, `XR_EXT_cocoa_window_binding`
- **OpenGL** (`compositor/gl/`) — Shipping. Windows + macOS
- **Vulkan** (`compositor/vk_native/`) — Shipping. Windows + macOS (MoltenVK)

Why native compositors instead of Vulkan interop: `docs/adr/ADR-001-native-compositors-per-graphics-api.md`.
Compositor never weaves — that's the DP's job: `docs/adr/ADR-007-compositor-never-weaves.md`.

### Custom OpenXR Extensions
- `XR_EXT_win32_window_binding` — App passes HWND to runtime
- `XR_EXT_cocoa_window_binding` — App passes NSWindow to runtime
- `XR_EXT_display_info` — Display dimensions, eye tracking modes
- `XR_EXT_android_surface_binding` — Android surface binding

Full extension specs: `docs/specs/XR_EXT_display_info.md`, `docs/specs/XR_EXT_win32_window_binding.md`, `docs/specs/XR_EXT_cocoa_window_binding.md`.
Eye tracking MANAGED vs MANUAL contract: `docs/specs/eye-tracking-modes.md`.

## Development Notes

### Languages and Standards
- C11 for core code, C++17 where needed, Python 3.6+ for build scripts

### Running Without Installing
```bash
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

### Key CMake Options
- `XRT_HAVE_LEIA_SR` — LeiaSR SDK support
- `XRT_HAVE_LEIA_SR_VULKAN` / `XRT_HAVE_LEIA_SR_D3D11` — API-specific weavers
- `XRT_FEATURE_SERVICE` — Out-of-process service mode
- `BUILD_TESTING` — Test suite

### CMake Variable Notes
- `LEIASR_SDKROOT` — Required env var for LeiaSR SDK path
- `SR_PATH` — Internal, auto-set from `LEIASR_SDKROOT`

### GitHub Actions Build
**Windows** (`.github/workflows/build-windows.yml`):
- `LEIASR_SDKROOT` + `CMAKE_PREFIX_PATH` both needed
- Artifact: `DisplayXR`

**macOS** (`.github/workflows/build-macos.yml`):
- Vulkan SDK via MoltenVK, bundles libvulkan + OpenXR loader
- Artifact: `DisplayXR-macOS`

## Claude Code Skills

### /release - Tagged Release Pipeline
Creates a tagged release, monitors CI build and publish pipeline, verifies all public repos are updated. See `.claude/skills/release/SKILL.md`.
```
/release v1.0.0    # explicit version
/release patch     # auto-bump: v1.0.0 → v1.0.1
/release minor     # auto-bump: v1.0.0 → v1.1.0
```
Updates `CMakeLists.txt` version, creates tag, monitors `build-windows.yml` + `publish-public.yml`, verifies releases on `displayxr-runtime` and `displayxr-shell-releases`. Rolls back tag on build failure.

### /ci-monitor - Feature Branch CI Validation
Automates commit, push, GitHub Actions monitoring, auto-fix. See `.claude/skills/ci-monitor/SKILL.md`.
Use for feature branch (`**-ci`) validation. **Not for releases** — use `/release` instead.
**Important:** Always include the related GitHub issue number in commit messages — e.g., `Fix linker error (#93)`. Check conversation context and recent commits to determine the issue number.

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. See `~/.claude/skills/ask-gemini/SKILL.md`.

## macOS Test App Local Builds

Copy binaries to `_package/DisplayXR-macOS/bin/`. Run scripts exec from `$DIR/bin/`.

| Test App | Build Output | Package Binary | Run Script |
|----------|-------------|---------------|------------|
| cube_handle_vk_macos | `test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos` | `_package/.../bin/cube_handle_vk_macos` | `run_cube_handle_vk.sh` |
| cube_handle_metal_macos | `test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos` | `_package/.../bin/cube_handle_metal_macos` | `run_cube_handle_metal.sh` |
| cube_handle_gl_macos | `test_apps/cube_handle_gl_macos/build/cube_handle_gl_macos` | `_package/.../bin/cube_handle_gl_macos` | `run_cube_handle_gl.sh` |
| cube_texture_metal_macos | `test_apps/cube_texture_metal_macos/build/cube_texture_metal_macos` | `_package/.../bin/cube_texture_metal_macos` | `run_cube_texture_metal.sh` |
| cube_hosted_metal_macos | `test_apps/cube_hosted_metal_macos/build/cube_hosted_metal_macos` | `_package/.../bin/cube_hosted_metal_macos` | `run_cube_hosted_metal.sh` |
| cube_hosted_legacy_metal_macos | `test_apps/cube_hosted_legacy_metal_macos/build/cube_hosted_legacy_metal_macos` | `_package/.../bin/cube_hosted_legacy_metal_macos` | `run_cube_hosted_legacy_metal.sh` |
| gaussian_splatting_handle_vk_macos | `demos/gaussian_splatting_handle_vk_macos/build/gaussian_splatting_handle_vk_macos` | `_package/.../bin/gaussian_splatting_handle_vk_macos` | `run_gaussian_splatting_handle_vk.sh` |

## Windows Test App Local Builds

`scripts\build_windows.bat test-apps` builds test apps and generates run scripts in `_package/`. Each run script sets `XR_RUNTIME_JSON` to the dev build so the installed runtime (from CI) is not used.

**Standalone apps:**

| Test App | Run Script |
|----------|------------|
| cube_handle_d3d11_win | `_package\run_cube_handle_d3d11_win.bat` |
| cube_hosted_d3d11_win | `_package\run_cube_hosted_d3d11_win.bat` |
| cube_handle_d3d12_win | `_package\run_cube_handle_d3d12_win.bat` |
| cube_handle_gl_win | `_package\run_cube_handle_gl_win.bat` |
| cube_handle_vk_win | `_package\run_cube_handle_vk_win.bat` |

**Shell mode — single command (recommended):**

`displayxr-shell.exe` auto-starts the service, activates shell mode via IPC, launches apps with correct env vars, and monitors clients.

```
_package\bin\displayxr-shell.exe app1.exe app2.exe
```

Example with two cube apps:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Optional per-app window pose (`--pose x,y,z,width_m,height_m` before each app path):
```
_package\bin\displayxr-shell.exe --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```

**Shell mode — legacy multi-terminal (still works):**

| Terminal | Script | Description |
|----------|--------|-------------|
| 1 | `_package\run_shell_service.bat` | Starts `displayxr-service --shell` |
| 2 | `_package\run_shell_app.bat` | First app → slot 0 (left-upper, 40% of display) |
| 3 | `_package\run_shell_app.bat` | Second app → slot 1 (right-upper, 40% of display) |

**Shell controls:** Left-click=focus window, title bar drag=move window, edge drag=resize, Right-click=focus+forward to app, Double-click title bar=maximize/restore, Scroll=resize window, Ctrl+1-4=layout presets, TAB=cycle focus, DELETE=close app, ESC=dismiss shell, V=toggle 2D/3D, WASD/left-click-drag=app input. Title bar buttons: close (X), minimize (—). Spatial raycasting hit-test (eye→cursor→window plane in meters).

**When launching from Claude Code:** Use `displayxr-shell.exe` — it handles service auto-start, `XR_RUNTIME_JSON`, and `DISPLAYXR_SHELL_SESSION=1` automatically. Use `run_in_background: true` on the Bash tool call and `timeout: 600000`. See `docs/roadmap/shell-phase1-status.md` for the full test procedure.

## Documentation

See `docs/README.md` for a complete index. Key docs by task:

| When you need to... | Read |
|---|---|
| Understand layer boundaries (what goes where) | `docs/architecture/separation-of-concerns.md` |
| Add a new display vendor | `docs/guides/vendor-integration.md` |
| Understand multiview tiling / atlas layout | `docs/specs/multiview-tiling.md` |
| Understand extension API (display_info, window bindings) | `docs/specs/XR_EXT_display_info.md` |
| Know why an architectural decision was made | `docs/adr/` (10 ADRs) |
| Understand legacy vs extension app differences | `docs/architecture/extension-vs-legacy.md` |
| Understand eye tracking MANAGED/MANUAL contract | `docs/specs/eye-tracking-modes.md` |
| Add a new OpenXR extension | `docs/guides/implementing-extension.md` |
| Write a device driver | `docs/guides/writing-driver.md` |
| Understand Leia SR weaver internals (DX11, DX12, GL, Vulkan) | `docs/reference/LeiaWeaver.md` |
| Understand Kooima projection math | `docs/architecture/kooima-projection.md` |
| Understand the compositor pipeline | `docs/architecture/compositor-pipeline.md` |
| Understand the swapchain model / canvas | `docs/specs/swapchain-model.md` |
| Track shell implementation progress | `docs/roadmap/shell-tasks.md` |
| Shell Phase 2 plan and status | `docs/roadmap/shell-phase2-plan.md`, `shell-phase2-status.md` |
| Understand the 3D capture pipeline | `docs/roadmap/3d-capture.md` |
| Understand shell/runtime IPC contract | `docs/roadmap/shell-runtime-contract.md` |
| Understand the overall product vision | `docs/roadmap/spatial-desktop-prd.md` |

## Debug Logs

See `docs/reference/debug-logging.md` for full conventions.
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat

## Capturing Compositor Screenshots (Preferred)

The D3D11 service compositor supports file-triggered screenshots of its combined atlas (full-resolution SBS back buffer). This reads the D3D11 texture directly — no DPI issues, no PrintWindow limitations.

**Trigger:** Create `%TEMP%\shell_screenshot_trigger`. The compositor checks every frame, captures the atlas, writes `%TEMP%\shell_screenshot.png`, and deletes the trigger.

```bash
# 1. Clean old capture
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/shell_screenshot.png"
# 2. Trigger capture
touch "/c/Users/SPARKS~1/AppData/Local/Temp/shell_screenshot_trigger"
# 3. Wait for compositor to process
sleep 3
# 4. View result (3840x2160 SBS atlas)
```
Then use the Read tool on `C:\Users\SPARKS~1\AppData\Local\Temp\shell_screenshot.png`.

**Toggle launcher programmatically (Ctrl+L):** The shell uses RegisterHotKey with a message-only window. Toggle via PostMessage:
```powershell
powershell -Command "
Add-Type @'
using System;using System.Runtime.InteropServices;
public class ShellMsg{
[DllImport(\"user32.dll\",CharSet=CharSet.Ansi)] public static extern IntPtr FindWindowExA(IntPtr p,IntPtr a,string c,string t);
[DllImport(\"user32.dll\")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
'@
\$h=[ShellMsg]::FindWindowExA([IntPtr]::new(-3),[IntPtr]::Zero,'Static','DisplayXR Shell Msg')
[ShellMsg]::PostMessage(\$h,0x0312,[IntPtr]::new(2),[IntPtr]::Zero)
"
```

**Code location:** `comp_d3d11_service.cpp`, in `multi_compositor_render()`, just before `swap_chain->Present()`.

## Capturing Window Screenshots (Legacy — PrintWindow)

To visually inspect the shell or any app window without user interaction:

**Step 1: Find the window HWND** by process name:
```powershell
powershell -Command "Get-Process displayxr-service | Select-Object Id, MainWindowTitle, MainWindowHandle"
```

**Step 2: Capture the window** using PrintWindow API (replace `HWND_VALUE` with the handle from step 1):
```powershell
powershell -Command "Add-Type @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class WC2 {
    [DllImport(\"user32.dll\")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport(\"user32.dll\")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public static void Cap(long hwnd, string p) {
        IntPtr h = new IntPtr(hwnd);
        RECT r; GetWindowRect(h, out r);
        int w = r.R-r.L, ht = r.B-r.T;
        if (w <= 0 || ht <= 0) { Console.WriteLine(\"Bad size\"); return; }
        var b = new Bitmap(w, ht);
        var g = Graphics.FromImage(b);
        IntPtr dc = g.GetHdc();
        PrintWindow(h, dc, 2);
        g.ReleaseHdc(dc);
        b.Save(p);
        g.Dispose(); b.Dispose();
        Console.WriteLine(\"OK \"+w+\"x\"+ht);
    }
}
'@ -ReferencedAssemblies System.Drawing; [WC2]::Cap(HWND_VALUE, 'shell_capture.png')"
```

**Step 3: View the screenshot** with the Read tool:
```
Read shell_capture.png
```

**Notes:**
- The shell compositor window is owned by `displayxr-service.exe`, not the shell launcher
- Window title is typically `DisplayXR - D3D11 Native Compositor`
- `FindWindow` by title may fail due to encoding — use HWND from `Get-Process` instead
- The captured image shows the composited output (all app windows, chrome, background)

## Debugging Crashes on Windows (procdump + cdb)

For ACCESS_VIOLATION or other crashes in the runtime or test apps:

**Step 1: Capture a crash dump** using procdump (download from `https://live.sysinternals.com/procdump64.exe`):
```bash
# Launch app under procdump — catches first-chance exception and writes full dump
procdump64.exe -accepteula -e -ma -x . path/to/app.exe
```
For shell mode: start the service first (`displayxr-service.exe --shell`), set `XR_RUNTIME_JSON` and `DISPLAYXR_SHELL_SESSION=1`, then launch the app under procdump.

**Step 2: Analyze the dump** with cdb (installed with WinDbg):
```bash
CDB="/c/Program Files/WindowsApps/Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe/amd64/cdb.exe"
# Get crash stack trace
"$CDB" -z crash.dmp -c ".ecxr; kP 15; q"
# Disassemble around crash site (replace ADDR with return address from stack)
"$CDB" -z crash.dmp -c ".ecxr; ub ADDR L15; q"
# Dump memory at a pointer (e.g., vtable inspection)
"$CDB" -z crash.dmp -c ".ecxr; dqs ADDR L20; q"
# Check registers at crash
"$CDB" -z crash.dmp -c ".ecxr; r; q"
```

**Step 3: Map offsets to source** — Release builds lack PDBs. Use `ub` (unassemble backwards) to find the calling instruction pattern (e.g., `mov rax,[rbx+offset]; call rax` reveals a vtable call). Cross-reference the offset with struct definitions to identify the null field.

**Common patterns:**
- `call rax` with `rax=0` → null function pointer in a vtable or dispatch table
- VK dispatch table nulls → app's VK device missing extension functions; use `submit_fallback` path
- `ACCESS_VIOLATION writing 0x0` at `address 0x0` → calling through null function pointer (not a data write)
- Crash in `DisplayXRClient.dll` without symbols → use `DisplayXRClient+OFFSET` with `ub` to disassemble
