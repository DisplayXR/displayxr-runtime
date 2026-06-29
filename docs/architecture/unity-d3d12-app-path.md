# Unity D3D12 (and other engine) app path

How a **Unity** D3D12 app reaches the runtime. This is **not obvious** because Unity
is not a DisplayXR-native app — it uses its own OpenXR plugin and never calls
`XR_EXT_win32_window_binding` itself. The DisplayXR Unity plugin
(`unity-3d-display`, ships as `displayxr_unity.dll`) bridges the gap by **hooking
`xrCreateSession` and injecting a window binding pointing at a plugin-created
overlay window**. The same pattern applies to any general engine integrated via a
hook layer rather than purpose-built source.

> Cross-repo: the plugin source lives in `unity-3d-display/native~`
> (`displayxr_hooks.cpp`, `displayxr_win32.c`, `displayxr_standalone_d3d12.cpp`).
> The runtime side is this repo. For the plain in-process vs service split see
> [in-process-vs-service.md](in-process-vs-service.md); for app classes see
> [app-classes](../getting-started/app-classes.md).

## TL;DR

Unity D3D12 (standalone, the default) runs on the **same in-process native D3D12
compositor** as `cube_handle_d3d12_win`, as a `_handle`-class app — but it is *not*
a drop-in equivalent. Two things differ:
1. **Upstream binding:** the HWND handed to the runtime is a **plugin-created child
   overlay window**, supplied via a **hook-injected** `XR_EXT_win32_window_binding`
   — not a window the app made for the runtime, and not the IPC/service path.
2. **Submission model:** Unity is a **fixed-resolution, always-2-view
   legacy-compromise** app (per-eye swapchains sized once; `scaleXY` not applied per
   frame), whereas cube renders **adaptive per-mode tiles**. The compositor *code*
   (atlas blit / crop / DP weave) is shared, but it is fed differently.

## Two routes, selected by mode

| | Unity **standalone** (default) | Unity **under shell** |
|---|---|---|
| Plugin behavior | creates a **child overlay** window, injects its HWND | passes Unity's **top-level** HWND, no overlay |
| Runtime route | **in-process native** `comp_d3d12_compositor` | **client → IPC → D3D11 service** |
| Selector | `oxr_d3d12_native_compositor_supported` → true (not service mode) | `displayxr_is_shell_mode()` in plugin / `is_service_mode` in runtime |

The rest of this doc covers the **standalone (in-process native)** route, since
that is the non-obvious one. The shell route is the ordinary client/IPC path
(`client_d3d12_compositor` → `comp_d3d11_service`).

## Definitive flow (standalone)

### Plugin side (`unity-3d-display/native~`), at `xrCreateSession`
`displayxr_unity.dll` is an OpenXR API-layer hook — it intercepts
`xrGetInstanceProcAddr` and hooks `xrCreateSession` (`hooked_xrCreateSession`,
`displayxr_hooks.cpp`). Unity's own OpenXR plugin drives the session normally;
DisplayXR's hook sits in the middle and, **before** calling the real
`xrCreateSession`:

1. Detects `XR_TYPE_GRAPHICS_BINDING_D3D12_KHR` in the next-chain → selects the
   D3D12 backend.
2. Calls `displayxr_get_app_main_view()` (`displayxr_win32.c`), which **creates an
   overlay window** over Unity's main HWND:
   - **Default (opaque):** `CreateWindowExW(WS_EX_TRANSPARENT, …, WS_CHILD |
     WS_VISIBLE, owner = Unity's main HWND)` — a child overlay sized to Unity's
     client rect. **This is the "HWND with a child."**
   - **Transparent (#57):** a top-level `WS_POPUP | WS_EX_NOREDIRECTIONBITMAP`
     overlay; Unity is cloaked + moved off-screen; DComp alpha compositing.
3. **Injects** an `XrWin32WindowBindingCreateInfoEXT` onto the end of the
   `xrCreateSession` next-chain with `windowHandle = the overlay HWND` (plus a
   `readbackCallback` and the `transparentBackgroundEnabled` flag).
4. Calls the real `xrCreateSession`.

### Runtime side (this repo)
With `xsi->external_window_handle` now set to the overlay HWND:

1. `oxr_session.c` (D3D12 binding branch) → `oxr_d3d12_native_compositor_supported`
   returns true (not service mode) → **`oxr_session_populate_d3d12_native`**
   (`oxr_session_gfx_d3d12_native.c`).
2. `comp_d3d12_compositor_create(xdev, hwnd = overlay, …)` is called **directly**:
   `sess->compositor = &xcn->base`, `is_d3d12_native_compositor = true`. **No
   client wrapper, no Vulkan, no IPC.**
3. In `comp_d3d12_compositor_create` it takes the `else if (hwnd != nullptr)`
   branch → `c->hwnd = overlay`, `owns_window = false`. So it is a
   **`_handle`-class app** (not hosted/self-created-window).
4. `comp_d3d12_target_create` runs `CreateSwapChainForHwnd` **on the overlay**.
   The overlay is a fresh window with no swapchain, so this **succeeds directly**
   — the runtime's own `E_ACCESSDENIED → WS_CHILD fallback`
   (`comp_d3d12_target.cpp`, `create_child_window_threaded`) is **not** triggered.
   The child in play is the *plugin's* overlay, not the runtime fallback.

### Frame loop — same compositor code, **different submission model**
The compositor *mechanics* are the shared D3D12 path, but Unity **feeds** them very
differently from a native handle app (see the next section — this is the part that
is easy to get wrong).

- Swapchain: `oxr_swapchain_d3d12_native_create` → `comp_d3d12_swapchain_create`
  (compositor-owned `ID3D12Resource` on Unity's device); images surfaced to Unity
  via `d3d12_native_enumerate_images` as `XrSwapchainImageD3D12KHR.texture`. Unity
  allocates **per-eye** swapchains **once** at session start (from
  `xrEnumerateViewConfigurationViews`) — or **one** swapchain with `arraySize=2`
  under Single-Pass-Instanced — and renders each eye at that **fixed** resolution
  every frame.
- `d3d12_compositor_layer_commit`: per-view atlas blit
  (`comp_d3d12_renderer.cpp:draw_projection_pass`) → **crop to content**
  (`d3d12_crop_atlas_for_dp`) → **DP weave** (`process_atlas`) into the overlay's
  back buffer → HUD → `comp_d3d12_target_present`. Composited pixels also flow
  back to Unity via the injected `readbackCallback`.
- **View config is `PRIMARY_STEREO` for *everyone* — that is not the difference.**
  The runtime only ever advertises `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`
  (`oxr_system.c`), and `xrLocateViews` always returns `view_count` = the **MAX
  across all modes** (e.g. 4 — `sim_display`'s Quad mode), not 2 (`oxr_session.c`;
  `xrEndFrame` then accepts a projection `viewCount` matching *any* mode's
  `view_count` — `oxr_session_frame_end.c`).
  - A **native handle app (cube)** renders the active mode's **N views as tiles
    into one worst-case-sized swapchain** (multiview tiling, ADR-010) and submits an
    N-view projection layer with per-tile `imageRect`.
  - A **built Unity app** renders **only 2 eyes** (per-eye swapchains, or SPI
    `arraySize=2`) and submits a **2-view** layer. **There is no view synthesis
    downstream** — nothing reconstructs additional views from those 2 eyes, so a
    built Unity app can only drive modes whose `view_count ≤ 2`; a mode with more
    views (e.g. `sim_display`'s 2×2 Quad, `view_count = 4`) requires the app to
    render all N tiles itself, which Unity cannot. (The runtime *does* synthesise
    per-tile **viewer poses** — not pixels — so a native/extension app can render
    the N views; see `sim_display_device.c`. True N-view *rendering* in the Unity
    stack exists only in the plugin's editor standalone preview, which runs its own
    hookless OpenXR session.)
  So a 2D↔3D mode switch does **not** change Unity's render resolution — only the
  atlas tiling / `imageRect` it's composited into.
- **SPI:** when Unity uses `arraySize=2`, the eyes are array **layers** 0/1
  submitted with `subImage.imageArrayIndex`; the per-view SRV must sample the layer
  named by `imageArrayIndex` (`FirstArraySlice = view.subImage.array_index`), not a
  hardcoded layer 0.

## How this differs from a native D3D12 app (`cube_handle_d3d12_win`) — and why

| | `cube_handle_d3d12_win` | Unity D3D12 (standalone) |
|---|---|---|
| Who calls `xrCreateSession` | the app itself | Unity's OpenXR plugin; **DisplayXR hooks it** |
| `XR_EXT_win32_window_binding` | app fills it with its own HWND | plugin **injects** it |
| HWND handed to the runtime | a clean window made **for the runtime** | a **child overlay** over Unity's main window |
| Window ownership | app owns one window; nothing else presents to it | Unity owns + presents to its **main** window; overlay is a *second* window |
| Readback | none | `readbackCallback` returns composited pixels |
| View configuration type | `PRIMARY_STEREO` | `PRIMARY_STEREO` (same — *not* the difference) |
| Views the app renders | the active mode's **N** (e.g. 2, or 4 in Quad) as **tiles in one** worst-case swapchain | **always 2** eyes; **no view synthesis** → limited to `view_count ≤ 2` modes |
| Per-view render resolution | **adaptive** — each view at `window × scaleXY` of the active mode | **fixed** — per-eye swapchains sized once at session start; `scaleXY` not applied per frame (ADR-006 "legacy compromise") |
| Compositor code (atlas blit / crop / DP weave) | shared | shared (fed differently) |

Note: the compositor *code path* is shared, but the *submission model* is not.
A native handle app renders adaptive per-mode tiles; Unity is effectively a
**fixed-resolution, always-2-view legacy-compromise** app whose per-mode tiling is
reconciled by the compositor at `layer_commit`/`xrEndFrame`. (On the **D3D11**
service path Unity reconciles the atlas itself in `displayxr_d3d11_backend.cpp`; on
**D3D12** the runtime compositor does the per-view blit.) The plugin also no longer
computes Kooima — it chains an `XR_EXT_view_rig` descriptor onto `xrLocateViews`
and consumes the runtime's render-ready views.

**Why the overlay is necessary** (all because Unity is a general engine, not a
bespoke DisplayXR app):

1. **Unity already owns its main HWND and presents its own swapchain to it.**
   DXGI enforces one swapchain per HWND for D3D12 — handing Unity's main HWND to
   the runtime would `E_ACCESSDENIED` (the exact case the runtime's child-window
   fallback exists for). The plugin sidesteps it with a fresh overlay window.
2. **Visual conflict.** Even if sharing were allowed, Unity continuously presents
   its flat game frame to its main window; the runtime's interlaced weave output
   would fight it. A separate overlay **stacked on top** lets the runtime own the
   screen while Unity's own present stays hidden behind it.
3. **Unity won't emit the window binding.** Unity's OpenXR plugin knows nothing
   about `XR_EXT_win32_window_binding` / 3D-display weaving. The only way to get
   the binding + the right HWND into `xrCreateSession` is the hook + injection.

A cube app has none of these constraints: it's written for DisplayXR, creates one
clean window expressly for the runtime, fills the binding itself, and never
presents to that window — no hook, no overlay, no conflict.
