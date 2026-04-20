# Production Components

What ships, what runs, and how the pieces connect.

## Components

The DisplayXR installer delivers four artifacts:

| Component | Binary | What it does |
|-----------|--------|-------------|
| **Runtime DLL** | `DisplayXRClient.dll` | OpenXR API implementation. Loaded in-process by every OpenXR app. |
| **Service** | `displayxr-service.exe` | IPC server + multi-compositor. Hosts the display for sandboxed apps and multi-app shell sessions. |
| **Shell** | `displayxr-shell.exe` | Spatial window manager. Arranges 3D and 2D apps in a shared 3D scene with window chrome, layout presets, and an app launcher. |
| **WebXR Bridge** | `displayxr-webxr-bridge.exe` | Metadata sideband for Chrome. Gives WebXR pages access to display info, rendering modes, and eye poses that Chrome's native WebXR path doesn't expose. |

## Two Compositor Paths

The runtime DLL (`DisplayXRClient.dll`) decides at load time whether to composite in-process or delegate to the service. This is the most important architectural branch in the system.

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

**This path is used by:** all handle, texture, and hosted apps running outside a sandbox and outside the shell.

### IPC (service) — sandboxed and shell-managed apps

```
App (sandboxed or shell-launched)
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

**This path is used by:** Chrome/Edge WebXR (AppContainer sandbox), apps launched by the shell (`DISPLAYXR_SHELL_SESSION=1`), and apps explicitly forced via `XRT_FORCE_MODE=ipc`.

### How the DLL decides

The decision happens in `u_sandbox_should_use_ipc()` (`src/xrt/auxiliary/util/u_sandbox.c`):

1. **`XRT_FORCE_MODE=native`** → in-process (override)
2. **`XRT_FORCE_MODE=ipc`** → IPC (override)
3. **`DISPLAYXR_SHELL_SESSION=1`** → IPC (set by shell at launch)
4. **AppContainer / sandbox detected** → IPC (automatic)
5. **Otherwise** → in-process

On Windows, sandbox detection queries `TokenIsAppContainer` on the process token. On macOS, it calls `sandbox_check()`. This means Chrome and UWP apps automatically route through IPC without any configuration.

## How the Components Connect

### Standalone native app (no service, no shell)

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
- **Metadata path:** Chrome extension → WebSocket → bridge process → its own OpenXR session with `XR_EXT_display_info` enabled. This carries display geometry, rendering modes, eye poses, and input — things Chrome's native WebXR path doesn't expose.

The bridge is a separate binary because Chrome's WebXR implementation doesn't support vendor extensions. The extension injects a `session.displayXR` API surface into the page's WebXR session via a navigator.xr Proxy in the MAIN content script world.

### Shell mode (service required)

```
                        ┌─── 3D app A ──► DLL (IPC) ──┐
                        │                              │
Shell ── IPC ──► Service ◄─── 3D app B ──► DLL (IPC) ──┤──► Multi-compositor ──► Display
                        │                              │
                        └─── 2D app C ── HWND capture ─┘
```

The shell is a privileged IPC client that:
1. Activates multi-compositor mode on the service
2. Launches 3D apps with `DISPLAYXR_SHELL_SESSION=1` (forces IPC)
3. Captures 2D desktop windows via `Windows.Graphics.Capture`
4. Sends window poses, focus, and layout commands over IPC

The service composites all clients — 3D OpenXR apps and captured 2D windows — into a single spatial scene with per-window Kooima projection.

## What Starts When

### At install

The installer registers:
- `DisplayXR_win64.json` as the active OpenXR runtime (`HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`)
- Service in the Windows logon Run key (`HKLM\...\Run\DisplayXR Service`)
- Start Menu shortcuts for shell and switcher

### At Windows logon

The **service** auto-starts via the Run key. It sits in the system tray with near-zero CPU, listening for IPC connections. This is necessary because Chrome's AppContainer sandbox blocks on-demand service launch (`ACCESS_DENIED` on `CreateProcess`). Without pre-launch, WebXR would silently fail.

The **shell** and **bridge** do not auto-start.

### On demand

| Trigger | What starts |
|---------|------------|
| Native app calls `xrCreateInstance()` | Nothing new — DLL composites in-process |
| Chrome opens a WebXR page | Service already running; app connects via IPC |
| User launches shell (Start Menu or shortcut) | Shell starts, auto-launches service in shell mode if not running |
| User opens WebXR page with extension | User must manually start bridge (or extension shows connection error) |

## Key Files

| Area | File | Purpose |
|------|------|---------|
| Mode decision | `src/xrt/auxiliary/util/u_sandbox.c` | `u_sandbox_should_use_ipc()` — the branch point |
| Hybrid entry | `src/xrt/targets/openxr/target.c` | `xrt_instance_create()` — picks native vs IPC |
| Service entry | `src/xrt/targets/service/main.c` | Service process with tray icon and IPC mainloop |
| Shell entry | `src/xrt/targets/shell/main.c` | Shell process with hotkeys, launcher, capture |
| Bridge entry | `src/xrt/targets/webxr_bridge/main.cpp` | WebSocket server + headless OpenXR client |
| Installer | `installer/DisplayXRInstaller.nsi` | NSIS script — registry, Run key, shortcuts |
| IPC security | `src/xrt/ipc/server/ipc_server_mainloop_windows.cpp` | Named pipe DACL (AppContainer access) |

## Further Reading

- [In-Process vs Service](in-process-vs-service.md) — deep dive into D3D11 compositor internals (swapchain sharing, eye tracking pipeline, KeyedMutex)
- [App Classes](../getting-started/app-classes.md) — the four app integration modes (handle, texture, hosted, IPC)
- [Separation of Concerns](separation-of-concerns.md) — layer boundaries and what each layer owns
- [Shell/Runtime Contract](../roadmap/shell-runtime-contract.md) — IPC protocol between shell and service
- [MCP Spec v0.2](../roadmap/mcp-spec-v0.2.md) — AI-native runtime control over Model Context Protocol
