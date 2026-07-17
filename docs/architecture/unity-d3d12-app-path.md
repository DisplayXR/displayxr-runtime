# Unity D3D12 (and other engine) app path

How a **Unity** app reaches the runtime. This is **not obvious** because Unity is not a
DisplayXR-native app — it drives OpenXR through its own XR pipeline and never calls
`XR_DXR_win32_window_binding` itself. The DisplayXR Unity plugin bridges the gap.

> **Paradigm change (2026, epic #166 / PR #177).** The plugin was rewritten from an
> **OpenXR API-layer hook** (which intercepted `xrGetInstanceProcAddr`, hooked
> `xrCreateSession`, and *injected* a window binding pointing at a plugin-created child
> overlay) to a first-class **`IUnityXRDisplay` display provider** — the same integration
> route Oculus/Varjo/Cardboard use. The old hook path and its standalone-D3D12 preview were
> **hard-removed**; this doc describes the provider. If you're reading old notes that mention
> `displayxr_hooks.cpp` / `hooked_xrCreateSession` / injected `WS_CHILD` overlays, that code
> is gone.

> Cross-repo: the plugin lives in **`displayxr-unity`** (UPM `com.displayxr.unity`); provider
> internals are documented there in
> [`docs~/architecture/xr-display-provider.md`](https://github.com/DisplayXR/displayxr-unity/blob/main/docs~/architecture/xr-display-provider.md).
> The runtime side is this repo. For the in-process-vs-service split see
> [in-process-vs-service.md](in-process-vs-service.md); for app classes see
> [app-classes](../getting-started/app-classes.md).

## TL;DR

The provider makes a shipping Unity player behave like the native `cube_handle_d3d12_win`
handle app: **Unity's XR-Management "DisplayXR Display" provider itself calls `xrCreateSession`**
(no hook), passing Unity's device on a `XrGraphicsBindingD3D12KHR`/`…D3D11KHR` next-chained to
an `XrWin32WindowBindingCreateInfoDXR`. It is still *not* a drop-in cube equivalent, for one
reason that survives the rewrite: **submission model.** Unity submits a **fixed-resolution,
always-2-view** projection layer (per-eye swapchains sized once at session start, or one
`arraySize=2` swapchain under Single-Pass-Instanced; `scaleXY` not applied per frame), whereas
cube renders **adaptive per-mode N-view tiles**. The compositor code is shared; it is fed
differently.

## Two routes, selected by transport

| | Unity **standalone** (default) | Unity **under the DisplayXR Shell** |
|---|---|---|
| Provider window | creates its **own top-level `WS_POPUP`** overlay over Unity's client area, binds the runtime to it | should **not** create the overlay — binds Unity's hidden main HWND (or none) and submits as a client |
| Runtime route | **in-process native** `comp_d3d12_compositor` (self-weaves) | **client → IPC → D3D11 service** (`client_d3d12_compositor` → `comp_d3d11_service` multi-compositor tile) |
| Selector | `is_service_mode` false → `oxr_d3d12_native_compositor_supported` true | `DISPLAYXR_WORKSPACE_SESSION=1` (or `XRT_FORCE_MODE=ipc`) → `u_sandbox_should_use_ipc()` → `is_service_mode` true |

**The transport switch is entirely runtime-side and already correct.** The shell launches 3D
apps with `XR_RUNTIME_JSON` + `DISPLAYXR_WORKSPACE_SESSION=1`, unelevated, window hidden.
`u_sandbox_should_use_ipc()` (`src/xrt/auxiliary/util/u_sandbox.c`) reads that env var and forces
IPC, which sets `is_service_mode=true`; each `oxr_session_gfx_*_native.c` then **refuses the
in-process native compositor** when `is_service_mode` is set, so the state tracker falls through
to the IPC client compositor. Only on that path does the app self-register a multi-compositor
slot (`comp_d3d11_service.cpp`, workspace-mode-gated) and become a shell-composited **tile** —
an in-process self-weaving app never connects to the service and cannot be a tile.

> **Provider-side status.** The provider must cooperate with the under-shell route by **not**
> creating/showing its `WS_POPUP` overlay and by submitting frames as a client instead of
> presenting to a window it owns. The runtime routing is done; the provider's workspace mode is
> being wired in `displayxr-unity` (detect the workspace session via the existing
> `displayxr_is_shell_mode()`, which already reads `DISPLAYXR_WORKSPACE_SESSION`; skip the
> overlay; bind Unity's hidden main HWND or `NULL`). Open questions tracked there: whether the
> per-frame pump needs any atlas reconciliation on the `client_d3d12_compositor` path, and
> whether a window binding is wanted at all from a 3D-submit client.

## Submission model (applies to both routes)

- **View config is `PRIMARY_STEREO` for everyone — that is not the difference.** The runtime
  only advertises `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` (`oxr_system.c`); `xrLocateViews`
  returns `view_count` = the **max across all modes** (e.g. 4 for `sim_display`'s Quad), and
  `xrEndFrame` accepts a projection `viewCount` matching *any* mode's `view_count`.
- A **native handle app (cube)** renders the active mode's **N views as tiles** in one
  worst-case-sized swapchain (multiview tiling, ADR-010) and submits an N-view projection layer
  with per-tile `imageRect`.
- A **built Unity app renders only 2 eyes** (per-eye swapchains, or SPI `arraySize=2`) and
  submits a **2-view** layer. **There is no view synthesis downstream** — nothing reconstructs
  additional views from those 2 eyes, so a built Unity app can only drive modes whose
  `view_count ≤ 2` (fine for typical 3D-display 3D = 2 views; a `sim_display` 2×2 Quad,
  `view_count = 4`, cannot be filled). The runtime synthesises per-tile **viewer poses** (not
  pixels), which is what lets native/extension apps render the N views.
- **SPI:** when Unity uses `arraySize=2`, the eyes are array **layers** 0/1 submitted with
  `subImage.imageArrayIndex`; the per-view SRV must sample the layer named by `imageArrayIndex`
  (`FirstArraySlice = view.subImage.array_index`), not a hardcoded layer 0. (SPI is auto-gated
  by runtime version — it needs runtime ≥ 1.26.1, else the plugin falls back to MultiPass.)
- The plugin does **not** compute Kooima — it chains an `XR_DXR_view_rig` descriptor onto
  `xrLocateViews` and consumes the runtime's render-ready views.

## How this differs from a native D3D12 app (`cube_handle_d3d12_win`)

| | `cube_handle_d3d12_win` | Unity D3D12 (provider) |
|---|---|---|
| Who calls `xrCreateSession` | the app itself | Unity's DisplayXR **display provider** (no hook) |
| `XR_DXR_win32_window_binding` | app fills it with its own HWND | provider fills it (overlay HWND standalone; hidden Unity HWND / none under shell) |
| Window handed to the runtime | a clean window made **for the runtime** | standalone: a provider-created **top-level `WS_POPUP`** overlay; under shell: Unity's hidden main window (or none) |
| Views the app renders | the active mode's **N** as tiles in one swapchain | **always 2** eyes; no view synthesis → limited to `view_count ≤ 2` modes |
| Per-view render resolution | **adaptive** — each view at `window × scaleXY` of the active mode | **fixed** — per-eye swapchains sized once at session start (ADR-006 "legacy compromise") |
| Compositor code (atlas blit / crop / DP weave) | shared | shared (fed differently) |

A cube app is written for DisplayXR: it creates one clean window expressly for the runtime,
fills the binding itself, and never presents to that window. The provider instead adapts Unity's
general-engine ownership of its own window — standalone it stacks a runtime-owned overlay over
Unity's frame; under the shell it yields the window entirely and submits as a client so the
service composites it as a tile alongside every other app.
