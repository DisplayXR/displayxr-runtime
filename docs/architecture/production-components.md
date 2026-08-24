# Production Components

What ships, what runs, and how the pieces connect.

## Components

The DisplayXR runtime installer delivers the runtime DLL, the service,
`displayxr-cli.exe`, `displayxr-control-panel.exe`, and the vendor-neutral
sim-display plug-in. The workspace controller (reference: DisplayXR Shell) ships
from `displayxr-shell-pvt` as a **separate** installer; the WebXR bridge is built
from this repo and installed with the runtime. The MCP adapter and the voice
agent ship separately again. The four rows below are the pieces that matter
architecturally, not one installer's file list:

| Component | Binary | What it does |
|-----------|--------|-------------|
| **Runtime DLL** | `DisplayXRClient.dll` | OpenXR API implementation. Loaded in-process by every OpenXR app. |
| **Service** | `displayxr-service.exe` | IPC server + multi-compositor. Hosts the display for sandboxed apps and multi-app workspace sessions. |
| **Workspace controller** (reference: **DisplayXR Shell**) | `displayxr-shell.exe` | Spatial window manager. Privileged IPC client that arranges 3D and 2D apps in a shared 3D scene with window chrome, layout presets, and an app launcher. The runtime exposes the workspace primitives (window pose, focus, capture) via `XR_DXR_spatial_workspace`; any privileged client implementing those extensions can replace the reference shell — for verticals, kiosks, OEM-branded workspaces, or AI-agent drivers. |
| **WebXR Bridge** | `displayxr-webxr-bridge.exe` | Metadata sideband for Chrome. Gives WebXR pages access to display info, rendering modes, and eye poses that Chrome's native WebXR path doesn't expose. |

## Two Compositor Paths

The runtime DLL (`DisplayXRClient.dll`) decides at load time whether to composite in-process or delegate to the service. This is the most important architectural branch in the system.

For the service side of that branch as built — processes, threads, locks, client
classes, failure domains, limits — see
[Service architecture](service-architecture.md). Note that on Windows the
service compositor is `comp_d3d11_service`, **not** `compositor/multi/`, and it
has two structurally different modes: workspace / multi-comp (service-owned
window, one composited output) and standalone per-client (each IPC client gets
its own swap chain and display processor). Both are described there.

### In-process (native) — most apps

```
App (D3D11 / D3D12 / Metal / GL / Vulkan)
  │
  └─► DisplayXRClient.dll
        │
        └─► Native compositor (in app's process, on app's GPU device)
              │
              └─► Display processor (vendor weaver) ──► Display
```

The app, compositor, and display processor all live in one process. No service needed. The compositor uses the app's own graphics device (AddRef'd). Swapchain textures are local — no cross-process sharing.

**This path is used by:** all handle, texture, and hosted apps running outside a sandbox and outside any active workspace.

### IPC (service) — sandboxed and workspace-managed apps

```
App (sandboxed or workspace-launched)
  │
  └─► DisplayXRClient.dll
        │
        └─► IPC client compositor
              │
              └─► Named pipe ──► displayxr-service.exe
                                    │
                                    └─► Multi-compositor (imports shared textures from N clients)
                                          │
                                          └─► Display processor ──► Display
```

The app gets a thin IPC client instead of a real compositor. Swapchain textures are shared cross-process via DXGI NT handles + KeyedMutex. The service owns the display and composites all clients into a single output.

**This path is used by:** Chrome/Edge WebXR (AppContainer sandbox), apps launched by a workspace controller (`DISPLAYXR_WORKSPACE_SESSION=1`), and apps explicitly forced via `XRT_FORCE_MODE=ipc`.

### How the DLL decides

The decision happens in `u_sandbox_should_use_ipc()` (`src/xrt/auxiliary/util/u_sandbox.c`):

1. **`XRT_FORCE_MODE=native`** → in-process (override)
2. **`XRT_FORCE_MODE=ipc`** → IPC (override)
3. **`DISPLAYXR_WORKSPACE_SESSION=1`** → IPC (set by workspace controller at launch)
4. **AppContainer / sandbox detected** → IPC (automatic)
5. **Otherwise** → in-process

On Windows, sandbox detection queries `TokenIsAppContainer` on the process token. On macOS, it calls `sandbox_check()`. This means Chrome and UWP apps automatically route through IPC without any configuration.

## How the Components Connect

### Standalone native app (no service, no workspace)

```
Native app ──► DisplayXRClient.dll ──► Native compositor ──► Display
```

The simplest case. App loads the DLL, gets an in-process compositor, talks directly to the display hardware. The service doesn't need to be running. This is the path for Unity games, Unreal apps, native handle apps, and any desktop app that isn't sandboxed.

### Chrome WebXR (service required)

```
Chrome tab (WebXR JS) ──────────────────── OpenXR ──── IPC ────► Service ──► Display
                                                                    ▲
Chrome extension ──── WebSocket (127.0.0.1:9014) ──── WebXR Bridge ─┘
                      (display_info, eye poses,         (headless OpenXR client,
                       mode changes, input)              metadata only — no frames)
```

Two separate connections to the service:

- **Frame path:** Chrome's built-in WebXR → OpenXR loader → `DisplayXRClient.dll` (IPC mode, AppContainer detected) → service compositor. This carries frames. Zero-copy on DXGI shared handles.
- **Metadata path:** Chrome extension → WebSocket → bridge process → its own OpenXR session with `XR_DXR_display_info` enabled. This carries display geometry, rendering modes, eye poses, and input — things Chrome's native WebXR path doesn't expose.

The bridge is a separate binary because Chrome's WebXR implementation doesn't support vendor extensions. The extension injects a `session.displayXR` API surface into the page's WebXR session via a navigator.xr Proxy in the MAIN content script world.

### Workspace mode (service required)

```
                        ┌─── 3D app A ──► DLL (IPC) ──┐
                        │                              │
Workspace ─ IPC ─► Service ◄─── 3D app B ──► DLL (IPC) ──┤──► Multi-compositor ──► Display
                        │                              │
                        └─── 2D app C ── HWND capture ─┘
```

A workspace controller is a privileged IPC client that:
1. Activates workspace mode on the service via `workspace_activate`
2. Launches 3D apps with `DISPLAYXR_WORKSPACE_SESSION=1` (forces IPC)
3. Captures 2D desktop windows via `Windows.Graphics.Capture`
4. Sends window poses, focus, and layout commands via the `XR_DXR_spatial_workspace` extensions

The service composites all clients — 3D OpenXR apps and captured 2D windows — into a single spatial scene with per-window Kooima projection.

## What Starts When

### At install

The installer registers:
- `DisplayXR_win64.json` as the active OpenXR runtime (`HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`)
- Service in the Windows logon Run key (`HKLM\...\Run\DisplayXR Service`)
- Start Menu shortcuts for the reference shell + switcher

### At Windows logon

The **service** auto-starts via the Run key. It sits in the system tray with near-zero CPU, listening for IPC connections. This is necessary because Chrome's AppContainer sandbox blocks on-demand service launch (`ACCESS_DENIED` on `CreateProcess`). Without pre-launch, WebXR would silently fail.

The service is the always-on **orchestrator**, and by default it spawns its
children on demand. The shipped `service.json` defaults are `workspace=auto`
and `bridge=auto` (`service_config.c:37-38`):

- **`workspace=auto`** — the service installs a `WH_KEYBOARD_LL` Ctrl+Space hook
  on its tray thread and spawns the registered workspace controller when the
  hotkey fires (`service_orchestrator.c:700-779`).
- **`bridge=auto`** — the service opens the :9014 trampoline at startup and
  spawns `displayxr-webxr-bridge.exe` on the extension's first connect
  (`service_orchestrator.c:527-638`).

The third mode, `enable`, spawns the child at service start and restarts it if it
exits. `disable` never spawns it.

Two further nuances: the client-side auto-launch of the service passes **no
arguments** — workspace mode is entered later by the `workspace_activate` IPC
call, not by how the service was launched — and a service autostarted from the
Run key with `--autostart` **exits immediately** when `start_on_login=false`
(`service/main.c:125`), a user-flippable tray toggle.

### On demand

| Trigger | What starts |
|---------|------------|
| Native app calls `xrCreateInstance()` | Nothing new — DLL composites in-process |
| Chrome opens a WebXR page | Service already running; app connects via IPC |
| User presses **Ctrl+Space** | Service spawns the registered workspace controller (`workspace=auto`) |
| User launches the workspace controller directly (Start Menu or shortcut) | Fallback path: the controller auto-launches the service if it isn't already running, then activates workspace mode over IPC |
| User opens a WebXR page with the extension | The extension's first connect to :9014 **is** the launch trigger — the service spawns the bridge (`bridge=auto`); no manual start |

## Key Files

| Area | File | Purpose |
|------|------|---------|
| Mode decision | `src/xrt/auxiliary/util/u_sandbox.c` | `u_sandbox_should_use_ipc()` — the branch point |
| Hybrid entry | `src/xrt/targets/openxr/target.c` | `xrt_instance_create()` — picks native vs IPC |
| Service entry | `src/xrt/targets/service/main.c` | Service process with tray icon and IPC mainloop |
| Workspace controller entry | `displayxr-shell-pvt/src/main.c` (private repo) | Reference workspace controller — hotkeys, launcher, 2D capture. This repo builds no shell binary. |
| Child-process orchestration | `src/xrt/targets/service/service_orchestrator.c` | Spawns and supervises the workspace controller; Ctrl+Space hook |
| Controller discovery | `src/xrt/targets/service/service_workspace_registry.c` | Enumerates registered workspace controllers and their published actions |
| Tray UI | `src/xrt/targets/service/service_tray_win.c` | Tray menu, published-actions rendering, Exit |
| Service config | `src/xrt/targets/service/service_config.c` | `service.json` — `workspace` child mode, `start_on_login` |
| Service compositor | `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | The Windows server compositor (workspace and standalone modes) |
| Installer | `installer/DisplayXRInstaller.nsi` | NSIS script — registry, Run key, shortcuts |
| IPC security | `src/xrt/ipc/server/ipc_server_mainloop_windows.cpp` | Named pipe DACL (AppContainer access) |

## Further Reading

- [Service architecture](service-architecture.md) — the service as built: processes, threads, locks, the two compositor modes, client classes, failure domains, limits
- [In-Process vs Service](in-process-vs-service.md) — deep dive into D3D11 compositor internals (swapchain sharing, eye tracking pipeline, KeyedMutex)
- [App Classes](../getting-started/app-classes.md) — the four app integration modes (handle, texture, hosted, IPC)
- [Separation of Concerns](separation-of-concerns.md) — layer boundaries and what each layer owns
- [Workspace/Runtime Contract](../roadmap/workspace-runtime-contract.md) — IPC protocol between a workspace controller and the service
- [MCP Spec](https://github.com/DisplayXR/displayxr-mcp/blob/main/docs/mcp-spec.md) — AI-native runtime control over Model Context Protocol (extracted to [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp))
