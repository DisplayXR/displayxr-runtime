# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

DisplayXR is a lightweight standalone OpenXR runtime purpose-built for 3D displays. The foundation work (issue #23) is complete — stripped from 500+ files to ~150, with native compositors for every major graphics API.

### Milestone Progress

See the [milestone tracker](https://github.com/DisplayXR/displayxr-runtime/milestones) for full status.

- **M1: Foundation** — Done. Stripped 34 VR drivers, removed Vulkan server compositor, cleaned CMake, extracted stereo math.
- **M2: Native Compositors** — Done. D3D11, D3D12, Metal, OpenGL, Vulkan all shipping.
- **M3: Test Coverage** — #30, #31, #33 open.
- **M4: Display Extensions** — Done. `XR_EXT_display_info` header frozen at v12 (#114 closed). Events (#3), multiview math (#38), eye tracking modes (#81), docs (#66) all complete. Vendor-initiated transition detection (#123) shipped via `get_hardware_3d_state()` DP vtable method. Remaining vendor work (MANUAL eye tracking mode) blocked on Leia SDK.
- **M5: Interface Standardization** — #45, #46, #47 open.
- **M6: Spatial Shell** — Done (Windows). Phases 0–8 shipped: multi-compositor, spatial windowing, window chrome, layout presets, 2D app capture, focus-adaptive 2D/3D mode, app launcher, graceful exit, 3D capture (Ctrl+Shift+C). macOS port deferred.
- **MCP (AI-Native Control)** — Framework lives in [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp) (extracted 2026-05). The runtime consumes it via CMake `FetchContent` (pin lives in `CMakeLists.txt`'s `FetchContent_Declare(displayxr_mcp ... GIT_TAG ...)` block; the full-stack pin matrix lives in `versions.json`) and registers Phase A handle-app introspection tools (`list_sessions`, `get_display_info`, `get_runtime_metrics`, `get_kooima_params`, `get_submitted_projection`, `diff_projection`, `capture_frame`, `tail_log`) inside each app's `libopenxr_displayxr` process. Phase B (workspace control: `list_windows`, `get/set_window_pose`, `set_focus`, `save/load_workspace`, `request_client_exit`) moved to `displayxr-shell-pvt`, which hosts an MCP server inside `displayxr-shell.exe` and wraps the public OpenXR workspace extension calls. The runtime no longer hosts an MCP endpoint in `displayxr-service`. Spec: [`displayxr-mcp/docs/mcp-spec.md`](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md).

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
- **Two distinct swapchains** — see `docs/specs/runtime/swapchain-model.md`
- **Canvas concept** — view dims and Kooima projection use canvas size, not display size. See `docs/specs/runtime/swapchain-model.md`.
- **Compositor pipeline** — see `docs/architecture/compositor-pipeline.md`

For the vendor isolation rule and layer "must NOT contain" constraints, see `docs/architecture/separation-of-concerns.md`.
For display processor vtable design (all 5 API variants), see `docs/archive/vendor-integration-historical.md`.

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
scripts\build_windows.bat installer  REM Runtime installer only
scripts\build_windows.bat test-apps  REM Test apps only (uses existing runtime build)
scripts\build_windows.bat generate   REM CMake generate only
```
Downloads all dependencies on first run (vcpkg, OpenXR loader). Requires VS 2022 with C++ workload, Ninja, Vulkan SDK, and GitHub CLI. Outputs to `_package/` (runtime) and `test_apps/*/build/` (test apps). Leia SR support comes from a separate installer — see "Vendor Plug-in Integration" below.

**The DisplayXR Shell ships from a separate repo
([displayxr-shell-pvt](https://github.com/DisplayXR/displayxr-shell-pvt))**
with its own installer (`DisplayXRShellSetup-*.exe`). The shell
installer requires the runtime as a hard prereq (reads
`HKLM\Software\DisplayXR\Runtime\InstallPath`), installs into the
runtime's tree, and registers itself at
`HKLM\Software\DisplayXR\WorkspaceControllers\shell` for the service
orchestrator to discover. The runtime owns no specific workspace app
— third-party verticals follow the same registration contract. See
`docs/specs/runtime/workspace-controller-registration.md`.

**When on a Windows machine with a Leia SR display, prefer local builds over CI** — iterate faster with `scripts\build_windows.bat build` and test directly. Run scripts are generated in `_package/` (see Windows Test App section below).

### Windows Compile-Check on macOS / Linux (MinGW-w64)
```bash
brew install mingw-w64        # one-time
./scripts/build-mingw-check.sh                # default targets: aux_util displayxr_mcp
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

**CI policy: contributor-friendly.** Public-repo CI is free on GitHub-hosted runners, so `build-windows.yml` and `build-macos.yml` fire on every PR (drafts included), every push to `main`, and every `v*` tag — favoring fast contributor feedback over minute-burn. (`displayxr-shell-pvt` is private and still uses the cost-saver draft-skip pattern; minutes there are billed.)

| Guard | Effect |
|---|---|
| **Doc-only PRs run a sentinel** | The workflow always fires, but `DetectChanges.outputs.docs_only` short-circuits the heavy steps. Runtime + Build (macOS) report success in ~30s on doc-only PRs without running the actual build. This pattern lets `Runtime` / `Build` / `shell-path-guard` be required-status-checks on branch protection without blocking doc-only PRs. |
| **Cancel-in-progress** | Rapid pushes to a PR cancel the in-progress CI run; only the latest commit's CI completes. `concurrency: group: ${{ workflow }}-${{ pr.number || ref }}, cancel-in-progress: true`. |
| **`shell-path-guard` (lint workflow)** | Fails any push or PR that reintroduces `src/xrt/targets/shell/*` or `installer/DisplayXRShellInstaller.nsi` — those moved to `displayxr-shell-pvt` during the privacy collapse and must not return to the public runtime tree. |

Net: doc-only PRs cost ~30s of CI. Code PRs run the full Windows + macOS build (free on public-repo runners).

For tagged releases, use the `/release` skill (see below) — it's the official release path. Don't tag manually.

### Standard CMake Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
```

### Leia SR support

Leia SR is no longer built in-tree (ADR-019, issues #256 & #263). To run on Leia hardware, install the runtime then install `DisplayXRLeiaSRSetup-*.exe` from [`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin/releases). The plug-in self-registers at `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` and the runtime's registry-driven discovery picks it up at `xrCreateInstance`.

### Running Tests
```bash
cd build && ctest
```

### Code Formatting
```bash
git clang-format    # Format only your changes (preferred)
scripts/format-project.sh   # Format all
```

### Releasing the Runtime

This repo IS the public runtime — there is no private→public mirror flow. A release is just a `vX.Y.Z` tag here, parallel Windows + macOS CI builds, and a GitHub Release with both installers attached (`DisplayXRSetup-*.exe` and `DisplayXR-Installer-*.pkg`) plus the test-apps bundle. macOS publishing post-#277.

```bash
# /release skill handles version bump, tagging, build monitoring, GH release create.
/release v1.2.1   # explicit version
/release patch    # auto-bump from latest v[0-9]+.[0-9]+.[0-9]+
/release minor
/release major
```

The skill's auto-bump regex is intentionally strict (`^v[0-9]+\.[0-9]+\.[0-9]+$`) so non-canonical tags (`v25.x` Monado-era leftovers cleaned 2026-05-04, `*-rc*`, `demo-x/v*`, `sr-sdk-*`) don't get picked up.

**Sibling release flows (NOT driven by /release in this repo):**

| Component | Repo | How it releases |
|---|---|---|
| Shell | `displayxr-shell-pvt` → `displayxr-shell-releases` | `/dxr-release` (user-level skill) or `git tag` → `publish-shell-releases.yml` builds + cross-publishes the installer + dispatches `versions-bump`. Authenticated via the `displayxr-publish-bot` GitHub App (org secrets `DISPLAYXR_APP_ID` + `DISPLAYXR_APP_PRIVATE_KEY`; local `.pem` backup at `.secrets/displayxr-publish-bot.pem`, see `.secrets/NOTE.md` for rotation). |
| Leia SR plug-in | `displayxr-leia-plugin` | `/dxr-release` or `git tag` → `build-windows.yml` builds DLL + installer + dispatches `versions-bump` with an ABI gate (ADR-020). Publishes `DisplayXRLeiaSRSetup-*.exe` (hard prereq: runtime installed). |
| MCP framework | `displayxr-mcp` | `/dxr-release` or `git tag` → matrix `build.yml` + adapter installer + dispatches `versions-bump`. Publishes `DisplayXRMCPSetup-*.exe`. |
| Extension headers | `displayxr-extensions` | Auto-syncs from `src/external/openxr_includes/` on every push to main via `publish-extensions.yml`. No tag needed. |
| Standalone demos | e.g. `displayxr-demo-gaussiansplat` | `/dxr-release` or `git tag` → `build-windows.yml` builds installer + dispatches `versions-bump`. Each demo's own repo + own tag cadence. |
| Meta-installer bundle | `displayxr-installer` | `/installer-release` (user-level skill) or `workflow_dispatch` on `publish-bundle.yml` — NOT auto-fired on component releases. Cuts a single `.exe`/`.pkg` that chains every component installer. |

### versions.json — single source of truth for the dev orchestrator + bundle

`versions.json` at the runtime repo root is the canonical pin matrix
that `scripts/setup-displayxr.{sh,bat}` consumes (and that
`displayxr-installer` mirrors byte-for-byte for its meta-installer
bundle composition).

**Auto-bumped on every component release** — the dispatch flow above
keeps it in lockstep with reality at no human cost. Tagging any
component (`/release` in runtime; `/dxr-release` in any sibling)
auto-updates the matching `versions.json` field on `main` AND
mirrors to `displayxr-installer/main` within ~30 s, via the
`displayxr-publish-bot` App.

Two structural safety nets:
- **ABI gate** (`scripts/check_plugin_abi.py`) — runs on any
  `field=leia_plugin` bump AND on every runtime self-bump. If the
  plug-in's reported ABI doesn't match the runtime's
  `XRT_PLUGIN_API_VERSION_CURRENT`, the bump is **skipped** and a
  tracking issue is auto-opened on `displayxr-leia-plugin`. Dev
  orchestrator stays on the last known-good bundle.
- **Drift guard** in `displayxr-installer/.github/workflows/publish-bundle.yml`
  — `assert-versions-in-sync` diffs the local `versions.json`
  against `runtime/main` via the GitHub Contents API (uncached)
  before any bundle build. Hand-edits to the installer's mirror
  copy are caught before users see them.

Full spec: [`docs/specs/runtime/versions-json-autobump.md`](docs/specs/runtime/versions-json-autobump.md).

### Repository Structure

| Repo | Visibility | Contents |
|------|-----------|----------|
| `DisplayXR/displayxr-runtime` | **Public** | Runtime source, all runtime dev issues, CI builds. **This repo.** |
| `DisplayXR/displayxr-runtime-legacy-mirror` | Public (archived) | Pre-deprivatize snapshot history; read-only. Old links auto-redirect from this repo. |
| `DisplayXR/displayxr-shell-pvt` | **Private** (dev) | DisplayXR Shell source, dev issues, CI |
| `DisplayXR/displayxr-shell-releases` | Public | DisplayXR Shell installer releases (auto-published from `displayxr-shell-pvt` on tags), user-facing bug reports |
| `DisplayXR/displayxr-leia-plugin` | **Public** | Leia SR display-processor plug-in source + `DisplayXRLeiaSRSetup-*.exe` releases. Extracted from this repo per issue #263 (ADR-019). |
| `DisplayXR/displayxr-extensions` | Public | OpenXR extension headers, auto-synced from this repo's `src/external/openxr_includes/` (consumed by shell-pvt + 3rd-party workspace apps) |
| `DisplayXR/displayxr-demo-<name>` | Public | Standalone demo repos with independent evolution. Currently `displayxr-demo-gaussiansplat`. No source-mirror from this repo. |

Shell source moved to `displayxr-shell-pvt` in 2026-05 (Phase 2.J Step 1). The runtime no longer carries any shell code; the discovery contract is documented at `docs/specs/runtime/workspace-controller-registration.md`.

### Issue Management

**Runtime dev issues** → `DisplayXR/displayxr-runtime` (this repo, public). **Shell dev issues** → `DisplayXR/displayxr-shell-pvt` (private). User-facing shell bug reports stay on the public `displayxr-shell-releases`.

| Where | What | Who |
|-------|------|-----|
| `DisplayXR/displayxr-runtime` | Runtime dev issues (bugs, tasks, implementation) | Developers + community |
| `DisplayXR/displayxr-shell-pvt` | Shell dev issues | Developers |
| `DisplayXR/displayxr-runtime` | Curated public runtime milestones only (~5-10 issues) | Public / OEMs |
| `DisplayXR/displayxr-shell-releases` | User-facing shell bug reports | Shell users |

**Rules:**
- Never dual-create issues across repos. One source of truth per issue.
- Create runtime dev issues on `DisplayXR/displayxr-runtime` (this repo).
- If a user files a bug on `displayxr-shell-releases`, triage it and create a dev issue on `displayxr-shell-pvt` if actionable.
- Community contributions: external contributors submit PRs directly against `DisplayXR/displayxr-runtime`. No two-hop apply step.

## Architecture

### Source Tree Structure (`src/xrt/`)
- **include/xrt/** — Core interface headers (`xrt_device.h`, `xrt_compositor.h`, `xrt_instance.h`, etc.)
- **auxiliary/** — Shared utilities: math (`m_*`), utilities (`u_*`), OS abstraction (`os_*`), Vulkan helpers (`vk_*`)
- **compositor/** — Native compositors (D3D11, D3D12, Metal, GL, Vulkan, multi, client, null). See `docs/architecture/project-structure.md`.
- **drivers/** — `sim_display/` (vendor-neutral simulation + plug-in), `qwerty/` (keyboard/mouse controllers). Vendor plug-ins (Leia SR etc.) ship from their own repos per ADR-019/#263.
- **state_trackers/oxr/** — OpenXR API implementation
- **ipc/** — Inter-process communication for service mode
- **targets/** — Build targets (runtime library, displayxr-cli, displayxr-service, displayxr-shell)

### Key Interfaces
C interfaces with vtable-style polymorphism:
- `struct xrt_device` — Abstract device interface
- `struct xrt_compositor` — Graphics compositor interface
- `struct xrt_instance` — Runtime instance
- `struct xrt_prober` — Device discovery

For the full interface catalog including display processor vtables (5 API variants), see `docs/archive/vendor-integration-historical.md`.

### Vendor Plug-in Integration

Vendor display drivers ship as **plug-in DLLs** from their own repos
(ADR-019 / issues #256 & #263). `DisplayXR-LeiaSR.dll`, built from
[`DisplayXR/displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)'s
`src/drv_leia/` with its `xrtPluginNegotiate` entry point, loads at
`xrCreateInstance` time via registry-driven discovery on Windows (or
JSON-manifest discovery on POSIX, as of #267 — macOS enforces vendor
separation via `DisplayXR-SimDisplay.dylib`) in
`target_plugin_loader.c`. The runtime DLL itself has zero SR or
`sim_display_*` identifiers in its link line — see ADR-019 §2.1 and
the CI assertion in `.github/workflows/build-windows.yml`.

- Eye tracking via the vendor SDK (inside the plug-in DLL).
- Display dimensions via `iface->get_display_info` →
  `xrt_plugin_display_info` (no direct runtime → vendor call).
- Plug-in discovery / registration contract:
  `docs/specs/runtime/plugin-discovery.md`.
- Vendor onboarding contract:
  `docs/guides/vendor-plugin-onboarding.md` (post-#263).

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

Full extension specs: `docs/specs/extensions/XR_EXT_display_info.md`, `docs/specs/extensions/XR_EXT_win32_window_binding.md`, `docs/specs/extensions/XR_EXT_cocoa_window_binding.md`.
Eye tracking MANAGED vs MANUAL contract: `docs/specs/vendor/eye-tracking-modes.md`.

## Development Notes

### Languages and Standards
- C11 for core code, C++17 where needed, Python 3.6+ for build scripts

### Running Without Installing

```bash
# Linux / macOS
XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json ./your_openxr_app
```

```cmd
:: Windows — run from a non-elevated terminal (see caveat below).
_package\run_cube_handle_d3d11_win.bat   :: sets XR_RUNTIME_JSON to dev manifest
```

**Elevated terminal caveat (Windows):** the bundled Khronos `openxr_loader.dll` (both 1.1.38 and 1.1.43) silently refuses `XR_RUNTIME_JSON` when the calling process is elevated (admin / UAC) and falls back to `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` → Program Files. No env-var override exists. Use a non-elevated terminal for dev iteration, or push the rebuilt DLL to Program Files.

**Verifying which DLL is loaded:** every `xrCreateInstance` logs one WARN line near the top of `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.<pid>_<ts>.log` with the absolute path of the actually-loaded `DisplayXRClient.dll` plus the value of `XR_RUNTIME_JSON`. Search for `loaded from:` — the path is authoritative.

Full reference: [`docs/getting-started/building.md` § Local Dev Iteration](docs/getting-started/building.md#local-dev-iteration).

### Key CMake Options
- `XRT_FEATURE_SERVICE` — Out-of-process service mode
- `BUILD_TESTING` — Test suite

Vendor display-processor code is **not** controlled by a CMake option
— all vendor drv_* implementations ship as plug-in DLLs from their
own repos (`DisplayXR-LeiaSR.dll` from `displayxr-leia-plugin`,
`DisplayXR-SimDisplay.dll` in-tree at `src/xrt/drivers/sim_display/`)
and are discovered at `xrCreateInstance` time per ADR-019/#263.

### GitHub Actions Build
**Windows** (`.github/workflows/build-windows.yml`):
- Builds without any vendor SDK (ADR-019/#263). Leia plug-in CI
  lives in `DisplayXR/displayxr-leia-plugin`.
- Artifact: `DisplayXR`

**macOS** (`.github/workflows/build-macos.yml`):
- Vulkan SDK via MoltenVK, bundles libvulkan + OpenXR loader
- Two jobs: `Build` (compile check) + `BuildInstaller` (full runtime + sim-display plug-in + .pkg, post-#277)
- Artifacts: `DisplayXR-Installer-macOS` (contains `DisplayXR-Installer.pkg`)
- BuildInstaller asserts the .pkg payload includes `DisplayProcessors/200-sim-display.json` + `lib/displayxr/plugins/DisplayXR-SimDisplay.dylib` (#274 regression guard)

## Claude Code Skills

### /release - Tagged Runtime Release (this repo only)
Creates a tagged runtime release here, monitors `build-windows.yml` + `build-macos.yml` in parallel, and attaches both installers (`DisplayXRSetup-*.exe` and `DisplayXR-Installer-*.pkg`) plus the test-apps bundle to the GitHub Release. See `.claude/skills/release/SKILL.md`.
```
/release v1.2.1    # explicit version
/release patch     # auto-bump from latest v[0-9]+.[0-9]+.[0-9]+
/release minor
/release major
```
Tags `main` HEAD and pushes the tag (no longer commits a "Release vX.Y.Z" bump to `main` — `build-windows.yml` patches `CMakeLists.txt` `VERSION` from `git describe --tags --abbrev=0 --match 'v[0-9]*'` at build time per PR #353, and `build-macos.yml` derives the version from `GITHUB_REF` directly, so the source-tree value would just drift). Watches Windows + macOS CI concurrently. The CI workflows attach artifacts to the GitHub Release on `v*` tag push via `softprops/action-gh-release@v2` (#290); the skill writes curated release notes on top via `gh release edit --notes`.

Post-CI, the skill also waits for `BumpVersionsJsonOnTag` (which runs the ABI gate against the currently-pinned leia plug-in, bumps `versions.json[runtime]` on `main`, and mirrors the file to `displayxr-installer/main`). Final report includes both bot-commit SHAs + any ABI-mismatch issue link if leia needs a rebuild.

macOS .pkg is a soft requirement — its absence warns but doesn't STOP the release. Only releases this repo — siblings use `/dxr-release` or `/installer-release` (below).

### /dxr-release - Sibling component release (user-level)
For every other DisplayXR component repo (`shell-pvt`, `leia-plugin`, `mcp`, `demo-*`). Detects the repo from `git remote`, tags HEAD, watches the repo's CI, watches the dispatched `versions-bump.yml` run in `displayxr-runtime`, reports the bump + installer-mirror outcome. Lives at `~/.claude/skills/dxr-release/SKILL.md`.
```
/dxr-release v1.2.3
/dxr-release patch
```
Does NOT apply to this repo (use `/release`) or to `displayxr-installer` (use `/installer-release`).

### /installer-release - Meta-installer bundle release (user-level)
For `displayxr-installer` only. `publish-bundle.yml` is `workflow_dispatch`-only (NOT auto-fired on component releases — bundle releases are deliberate). Skill pre-checks the versions.json drift guard locally, fires the dispatch, watches `assert-versions-in-sync` + `build-macos` + `build-windows` + `release`, reports. Lives at `~/.claude/skills/installer-release/SKILL.md`.
```
/installer-release v1.2.3
/installer-release v1.2.3 --prerelease
```

### /ask-gemini - Code Analysis with Gemini
Ask Gemini to analyze code and produce a read-only report. See `~/.claude/skills/ask-gemini/SKILL.md`.

## macOS Test App Local Builds

Copy binaries to `_package/DisplayXR-macOS/bin/`. Run scripts exec from `$DIR/bin/`. As of issue #267, every generated `run_*.sh` also sets `XRT_PLUGIN_SEARCH_PATH=$DIR/lib/displayxr/plugins` so the dev tree's `DisplayXR-SimDisplay.dylib` (alongside its `200-sim-display.json` manifest) is discovered by the runtime's plug-in loader without touching `~/Library/Application Support/`.

| Test App | Build Output | Package Binary | Run Script |
|----------|-------------|---------------|------------|
| cube_handle_vk_macos | `test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos` | `_package/.../bin/cube_handle_vk_macos` | `run_cube_handle_vk.sh` |
| cube_handle_metal_macos | `test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos` | `_package/.../bin/cube_handle_metal_macos` | `run_cube_handle_metal.sh` |
| cube_handle_gl_macos | `test_apps/cube_handle_gl_macos/build/cube_handle_gl_macos` | `_package/.../bin/cube_handle_gl_macos` | `run_cube_handle_gl.sh` |
| cube_texture_metal_macos | `test_apps/cube_texture_metal_macos/build/cube_texture_metal_macos` | `_package/.../bin/cube_texture_metal_macos` | `run_cube_texture_metal.sh` |
| cube_hosted_metal_macos | `test_apps/cube_hosted_metal_macos/build/cube_hosted_metal_macos` | `_package/.../bin/cube_hosted_metal_macos` | `run_cube_hosted_metal.sh` |
| cube_hosted_legacy_metal_macos | `test_apps/cube_hosted_legacy_metal_macos/build/cube_hosted_legacy_metal_macos` | `_package/.../bin/cube_hosted_legacy_metal_macos` | `run_cube_hosted_legacy_metal.sh` |

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

`displayxr-shell.exe` auto-starts the service, activates shell mode via IPC, launches apps with correct env vars, and monitors clients. The shell ships from [`displayxr-shell-pvt`](https://github.com/DisplayXR/displayxr-shell-pvt) via its own installer (`DisplayXRShellSetup-*.exe` from [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases)) — install it once and it lands at `C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe`. **This repo does NOT build a shell binary anymore** (the `_package\bin\displayxr-shell.exe` from old builds is stale; ignore / delete).

```
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" app1.exe app2.exe
```

Example with two cube apps:
```
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Optional per-app window pose (`--pose x,y,z,width_m,height_m` before each app path):
```
"C:\Program Files\DisplayXR\Runtime\displayxr-shell.exe" --pose -0.1,0.05,0,0.14,0.08 app1.exe --pose 0.1,0.05,0,0.14,0.08 app2.exe
```

**Shell mode — legacy multi-terminal (still works):**

| Terminal | Script | Description |
|----------|--------|-------------|
| 1 | `_package\run_shell_service.bat` | Starts `displayxr-service --shell` |
| 2 | `_package\run_shell_app.bat` | First app → slot 0 (left-upper, 40% of display) |
| 3 | `_package\run_shell_app.bat` | Second app → slot 1 (right-upper, 40% of display) |

**Shell controls:** Left-click=focus window, title bar drag=move window, edge drag=resize, Right-click=focus+forward to app, Double-click title bar=maximize/restore, Scroll=resize window, Ctrl+1-4=layout presets, TAB=cycle focus, DELETE=close app, ESC=dismiss shell, V=toggle 2D/3D, WASD/left-click-drag=app input. Title bar buttons: close (X), minimize (—). Spatial raycasting hit-test (eye→cursor→window plane in meters).

**When launching from Claude Code:** Use `displayxr-shell.exe` — it handles service auto-start, `XR_RUNTIME_JSON`, and `DISPLAYXR_WORKSPACE_SESSION=1` automatically. Use `run_in_background: true` on the Bash tool call and `timeout: 600000`. See `docs/roadmap/shell-phase1-status.md` for the full test procedure.

## Documentation

See `docs/README.md` for a complete index. Key docs by task:

| When you need to... | Read |
|---|---|
| Understand layer boundaries (what goes where) | `docs/architecture/separation-of-concerns.md` |
| Build a workspace app (shell-style controller) | `docs/specs/runtime/workspace-controller-registration.md` |
| Add a new display vendor | `docs/guides/vendor-plugin-onboarding.md` (post-#263 model — fork the Leia plug-in repo) then `docs/reference/xrt_plugin_iface.md` + `docs/specs/runtime/plugin-discovery.md` |
| Understand the `xrt_plugin_iface` callbacks | `docs/reference/xrt_plugin_iface.md` |
| Understand multiview tiling / atlas layout | `docs/specs/runtime/multiview-tiling.md` |
| Understand extension API (display_info, window bindings) | `docs/specs/extensions/XR_EXT_display_info.md` |
| Know why an architectural decision was made | `docs/adr/` (16 ADRs) |
| Understand legacy vs extension app differences | `docs/architecture/extension-vs-legacy.md` |
| Understand eye tracking MANAGED/MANUAL contract | `docs/specs/vendor/eye-tracking-modes.md` |
| Add a new OpenXR extension | `docs/guides/implementing-extension.md` |
| Write a device driver | `docs/guides/writing-driver.md` |
| Understand Leia SR weaver internals (DX11, DX12, GL, Vulkan) | `docs/vendors/leia/weaver.md` |
| Understand Leia transparency model (compose-under-bg + chroma-key) | `docs/vendors/leia/transparency.md` |
| Understand Kooima projection math | `docs/architecture/kooima-projection.md` |
| Understand the compositor pipeline | `docs/architecture/compositor-pipeline.md` |
| Understand the swapchain model / canvas | `docs/specs/runtime/swapchain-model.md` |
| Track shell implementation progress | `docs/roadmap/shell-tasks.md` |
| Shell Phase 2 plan | `docs/roadmap/shell-phase2-plan.md` |
| Find vendor-specific docs (Leia, sim_display) | `docs/vendors/<vendor>/README.md` |
| Understand the 3D capture pipeline | `docs/roadmap/3d-capture.md` |
| Understand workspace/runtime IPC contract | `docs/roadmap/workspace-runtime-contract.md` |
| Understand the overall product vision | `docs/roadmap/spatial-desktop-prd.md` |

## Debug Logs

See `docs/reference/debug-logging.md` for full conventions.
- Use U_LOG_W (WARN) only for one-off init, error, and lifecycle events
- Use U_LOG_I (INFO) for recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.)
- Never add per-frame U_LOG_W calls — they cause massive log bloat

## Capturing macOS Test-App Pixels Autonomously

`screencapture`, `CGWindowListCreateImage` (obsoleted in macOS 15), and
`osascript ... keystroke` all need TCC permissions Claude Code's parent
process doesn't have. They fail silently or with a generic "could not
create image" error.

Workaround: dump pixels from inside the test app via `stbi_write_png`.
`stb_image_write.h` already lives in `test_apps/common/` and
`_stbi_write_png` is linked into every macOS test app via the
`atlas_capture_*.mm` siblings, so just forward-declare with C linkage at
file scope of the `.mm` you want to instrument:

```cpp
extern "C" int stbi_write_png(char const*, int, int, int, void const*, int);
```

Then dump from the render loop, gated on a frame counter:

```cpp
static int n = 0;
if (n++ == 120) {  // ~2 s in, after throttled HUD section strings populate
    stbi_write_png("/tmp/hud_source.png", W, H, 4, pixels, rowPitch);
}
```

Run: `_package/DisplayXR-macOS/run_cube_handle_metal.sh > /tmp/log 2>&1 &`,
sleep ~6 s, `pkill -f cube_handle_metal_macos`, then `Read /tmp/hud_source.png`
— the Read tool renders the PNG inline.

Caveat: HUD section strings (`g_hudSessionText` etc.) are throttled to
~2 Hz, so frame-0 dumps are blank. Wait for frame ~120 or later.

For the rendered per-eye atlas (after compositor blit, before weaver) the
existing 'I' key path writes to
`~/Pictures/DisplayXR/<stem>-N_<cols>x<rows>.png` via
`dxr_capture::CaptureAtlasRegionMetal`. You can't trigger the 'I' key via
osascript without Accessibility, so the same in-app dump trick works —
call `CaptureAtlasRegionMetal` directly from a frame-counter gate.

**Compositor-side composited atlas (preferred):** the metal and GL native
compositors poll `/tmp/dxr_atlas_trigger` each frame. Touch the trigger
and the next frame writes the composited content region (cube + WS HUD
layers, post per-tile pass, pre display processor) to `/tmp/dxr_atlas.png`,
then unlinks the trigger.

```bash
rm -f /tmp/dxr_atlas.png /tmp/dxr_atlas_trigger
_package/DisplayXR-macOS/run_cube_handle_metal.sh > /tmp/log 2>&1 &
sleep 4 && touch /tmp/dxr_atlas_trigger && sleep 2
pkill -f cube_handle_metal_macos
# Read /tmp/dxr_atlas.png inline.
```

This shows the composited atlas (what would go to the SR weaver / sim_display)
which is what you want for verifying WS-layer composition. The vk_native
compositor doesn't have a trigger yet; for cube_handle_vk_macos rely on
visual inspection or the in-app `stbi_write_png` trick.

## Capturing Compositor Screenshots (Preferred)

The D3D11 service compositor supports file-triggered screenshots of its combined atlas (full-resolution SBS back buffer). This reads the D3D11 texture directly — no DPI issues, no PrintWindow limitations.

**Trigger:** Create `%TEMP%\workspace_screenshot_trigger`. The compositor checks every frame, captures the atlas, writes `%TEMP%\workspace_screenshot.png`, and deletes the trigger.

```bash
# 1. Clean old capture
rm -f "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot.png"
# 2. Trigger capture
touch "/c/Users/SPARKS~1/AppData/Local/Temp/workspace_screenshot_trigger"
# 3. Wait for compositor to process
sleep 3
# 4. View result (3840x2160 SBS atlas)
```
Then use the Read tool on `C:\Users\SPARKS~1\AppData\Local\Temp\workspace_screenshot.png`.

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
For shell mode: start the service first (`displayxr-service.exe --shell`), set `XR_RUNTIME_JSON` and `DISPLAYXR_WORKSPACE_SESSION=1`, then launch the app under procdump.

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
