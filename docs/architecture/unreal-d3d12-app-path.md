# Unreal Engine D3D12 app path

How an **Unreal Engine** D3D12 game reaches the runtime. This is the **opposite**
of the [Unity path](unity-d3d12-app-path.md): Unity is an OpenXR API-layer **hook**
that rides on Unity's own XR stack and submits a fixed 2-view legacy compromise;
Unreal is a **first-class UE render plugin** that **replaces** UE's stock
`FOpenXRHMD`, loads the runtime directly, and drives the **full adaptive per-mode
N-view native path** itself — zero-copy into the runtime's swapchain. On the native
axes (view count, per-view resolution, projection) a built Unreal game is closer to
`cube_handle_d3d12_win` than Unity is.

> Cross-repo: the UE plugin source lives in **`displayxr-unreal`** (modules
> `DisplayXRCore` / `DisplayXREditor` / `DisplayXRMaterials`). The runtime side is
> this repo. File:line citations below into `Source/…` are in that repo. The
> plugin's own design rationale lives in its `Docs/DisplayXR/adr/ADR-00{1,2,3}-*.md`.
> For the Unity contrast see [unity-d3d12-app-path.md](unity-d3d12-app-path.md);
> for app classes see [app-classes](../getting-started/app-classes.md).

## TL;DR

A built Unreal D3D12 game runs on the **in-process native D3D12 compositor** as a
`_handle`-class app, like the native cube — but unlike Unity it gets there by being
a real UE rendering plugin, not a hook. Three things define it:

1. **Direct runtime load (not UE's OpenXR plugin).** The plugin loads the runtime
   DLL itself and binds via `xrNegotiateLoaderRuntimeInterface`, getting the
   **in-process** compositor (weaving in UE's process) instead of routing through
   the OpenXR loader → IPC client.
2. **Adaptive N-view native rendering.** It reads the display's rendering modes and
   renders the active mode's **N tiles** at adaptive per-view resolution, with its
   own off-axis projection. **There is no view synthesis** — it renders every view
   itself (the same contract the native cube follows).
3. **Zero-copy atlas handoff.** UE and the runtime share **one D3D12 device**
   (UE's). The runtime's swapchain images are wrapped as UE render targets, and UE's
   renderer draws the atlas tiles **straight into the runtime's swapchain** — no copy.

The window binding is the **one** thing it shares with Unity: a `WS_CHILD` overlay
over the game window (DXGI's one-swapchain-per-HWND rule forces it for both engines).

## Two routes, selected by mode

| | UE **standalone** (default) | UE **under shell / forced-IPC** |
|---|---|---|
| Runtime route | **in-process native** `comp_d3d12_compositor` | client → IPC → D3D11 service |
| Swapchain write | **zero-copy** direct render into runtime image | private RT → cross-process raw copy |
| Selector | `OverrideCompositorHWND == nullptr` (game) | `XRT_FORCE_MODE=ipc` / `DISPLAYXR_WORKSPACE_SESSION` set |
| Status | working, hardware-validated | **black — WIP**, see [below](#shell--ipc-path-status-black) |

The rest of this doc covers the **standalone (in-process native)** route, which is
the production path. The shell/IPC route is described at the end.

## Editor preview is a different path

Don't conflate the packaged game with the in-editor preview. The editor preview is
a separate `SceneCapture2D`-based session, `FDisplayXRPreviewSession`, in the
**`DisplayXREditor`** module — it is **not** the runtime path. The branch point is
`FDisplayXRPlatform::OverrideCompositorHWND` (`DisplayXRCoreModule.cpp:240`, default
`nullptr`): editor native-PIE sets it to a raw-Win32 mirror window; **the packaged
game leaves it null**. A `WorldCtxTag()` helper tags every log `[GAME]`/`[EDITOR]`/
`[PIE]` (`DisplayXRDevice.cpp:36-43`). Everything below is the **`[GAME]`, override
null** path.

## Definitive flow (standalone)

### Plugin side (`displayxr-unreal`)

The plugin is a first-class HMD plugin, registered ahead of UE's own OpenXR HMD:

1. **Replaces UE's HMD.** It implements `IHeadMountedDisplayModule` and bumps the
   `DisplayXRCore` HMD priority **+10 above** `OpenXRHMD`/`SteamVR` so UE selects it
   (`DisplayXRCoreModule.cpp:119-129, 143`). The `.uplugin` declares only `XRBase` —
   **not** UE's `OpenXR` plugin — so UE's stock OpenXR stack never drives the session
   (their *ADR-001*).
2. **Loads the runtime directly (their ADR-001).** Rather than `openxr_loader.dll`
   (which would route to the IPC client), it resolves the active runtime manifest
   from `XR_RUNTIME_JSON` or the `…\Khronos\OpenXR\1\ActiveRuntime` registry value,
   `LoadLibraryExW`s the runtime DLL, and binds `xrGetInstanceProcAddr` via
   **`xrNegotiateLoaderRuntimeInterface`** (`DisplayXRSession.cpp:202-316`). The
   comment at `:173` is explicit: *"This gives us the in-process compositor (weaving
   happens in our process) rather than going through openxr_loader.dll →
   DisplayXRClient.dll (IPC)."*
3. **Drives real OpenXR.** Full `xrCreateInstance` / `xrGetSystem` / `xrCreateSession`
   (D3D12 graphics binding + Win32 window binding) / `xrCreateSwapchain` /
   `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` / `xrLocateViews`. It enables
   `XR_DXR_display_info`, `XR_DXR_win32_window_binding`, `XR_KHR_D3D12_enable`, and
   the atlas-capture extension. Because it enables `XR_DXR_display_info`, the runtime
   treats it as an **extension app**, not a legacy app (contrast Unity).
4. **Creates the window binding** — a `WS_CHILD` overlay over the game window (see
   [Window handling](#window-handling)), bound via the same
   `XR_DXR_win32_window_binding` the cube fills.

### Runtime side (this repo)

With the D3D12 graphics binding and the overlay HWND set on the `xrCreateSession`
chain, the runtime takes the in-process native branch:

- `oxr_d3d12_native_compositor_supported` returns true (not service mode) →
  `oxr_session_populate_d3d12_native` → `comp_d3d12_compositor_create(hwnd = overlay)`,
  `is_d3d12_native_compositor = true`. No client wrapper, no IPC. (Same selector the
  Unity standalone path hits.)
- Because UE passes its **own** D3D12 device + graphics queue at `xrCreateSession`,
  the compositor's swapchain `ID3D12Resource`s live on UE's device. UE wraps each as
  a UE render target via `RHICreateTexture2DFromResource` (`DisplayXRCompositor.cpp:492`,
  on the render thread) and surfaces them through `IStereoRenderTargetManager` — so
  UE's renderer writes directly into the runtime's images (their *ADR-002*,
  zero-copy). A dedicated compositor thread runs the `xrWaitFrame`/`Begin`/`End`
  handshake (UE's render thread must not block on `xrWaitFrame`'s vsync).

## Rendering model — adaptive per-mode N views (the key contrast with Unity)

Unlike Unity's fixed 2-view submission, UE renders the active mode's **N tiles**
itself:

- **View count is data-driven from the display's modes.** The plugin reads
  `xrEnumerateDisplayRenderingModesDXR` into a `FDisplayXRViewConfig`
  (`tileColumns`/`tileRows`/`viewScaleX`/`viewScaleY`), and
  `GetDesiredNumberOfViews()` returns `CachedViewConfig.GetViewCount()`
  (= `tileColumns × tileRows`), clamped to ≥ 2 only because UE requires ≥ 2 for stereo
  (`DisplayXRDevice.cpp:362-371`). So in a 4-view mode UE renders **4** views, not 2.
- **Per-view resolution is adaptive.** `AdjustViewRect` sizes each tile as
  `window × scaleX/Y` at its `(col, row)` (`DisplayXRDevice.cpp:321`), and the live
  window size is re-read each frame — so a mode switch changes UE's render geometry
  (contrast Unity's session-start-fixed eye size).
- **Own off-axis projection (their ADR-003).** Only the per-view eye *positions*
  cross the boundary; UE rebuilds the projection matrix in its own convention
  (reverse-Z, infinite-far) and returns it from `GetStereoProjectionMatrix`
  (`DisplayXRDevice.cpp:344`). The runtime's projection/near/far outputs are
  discarded.
- **No view synthesis, no DP reliance for view *count*.** UE submits an N-view
  `XrCompositionLayerProjection`, one `XrCompositionLayerProjectionView` per tile,
  whose `imageRect` matches UE's per-tile offsets (`DisplayXRCompositor.cpp:179-189`).
  Nothing downstream invents views — exactly the native-cube contract.

## Window handling

A built UE game owns and presents to its game window, so it hits the **same**
DXGI one-swapchain-per-HWND constraint Unity does, and solves it the **same** way:

- `bUseParentDirectly = (OverrideCompositorHWND != nullptr)` — null in a game, so it
  takes the child-window branch (`DisplayXRCompositor.cpp:265-269`).
- `CreateChildWindow()` makes a `WS_CHILD | WS_VISIBLE` window of class
  `DisplayXROverlay` over the game HWND (`DisplayXRCompositor.cpp:825-836`), and the
  **child** HWND (not the parent) is bound via `XR_DXR_win32_window_binding`
  (`SessionHWND = ChildHWND`, `:271`). The child's WndProc returns `HTTRANSPARENT`
  for hit-testing and the compositor keeps it sized to the parent each frame.

This is `_handle`-class behavior (a real app HWND), like Unity's overlay and unlike
`_hosted` (NULL → runtime self-creates a window). The runtime's own
`E_ACCESSDENIED → WS_CHILD` swapchain fallback (`comp_d3d12_target.cpp`) is **not**
what makes this child — the plugin creates it explicitly, just as the Unity plugin does.

## How this compares — UE vs Unity vs native cube

| | `cube_handle_d3d12_win` | **Unreal (standalone)** | Unity (standalone) |
|---|---|---|---|
| Integration kind | bespoke DisplayXR app | **first-class UE HMD plugin** | OpenXR API-layer **hook** |
| Loads runtime via | OpenXR loader → native | **direct `LoadLibraryExW` + `xrNegotiateLoaderRuntimeInterface`** | Unity's loader; hook injects binding |
| Views rendered | active mode's **N** as tiles | **N** (`cols × rows`), all rendered | **always 2** eyes |
| Per-view resolution | **adaptive** per mode | **adaptive** per mode (live window) | **fixed** at session start |
| View synthesis | none | **none** (renders all N) | none — but **can't drive `view_count > 2` modes** |
| Projection | runtime Kooima | **own off-axis** from runtime eye positions | runtime via chained `XR_DXR_view_rig` |
| `XR_DXR_display_info` | enabled (extension app) | **enabled** (extension app) | not enabled → legacy compromise |
| Window binding | clean runtime-owned HWND | game HWND → **`WS_CHILD` overlay** | main HWND → **`WS_CHILD`/`WS_POPUP` overlay** |
| Atlas assembly | runtime compositor | **UE renderer draws into runtime swapchain** (zero-copy) | D3D12: runtime blits / D3D11: plugin builds atlas |

On the native axes the ranking is **cube ≈ Unreal ≫ Unity**: UE renders all N views
adaptively with its own projection. Unity is the degraded one (fixed 2-view, limited
to `view_count ≤ 2` modes) because a hook can't change Unity's session-fixed swapchain.

## Known gap: >2-view eye/FOV in the submitted layer

UE renders each tile with a correct per-view projection (`GetStereoProjectionMatrix`),
and the submitted per-tile `imageRect` is correct, but the per-view **pose/FOV** in
the *submitted* projection layer are only filled for two eyes: view 0 = left eye, all
views ≥ 1 reuse the **right** eye with a hardcoded placeholder FOV
(`DisplayXRCompositor.cpp:183-186`). This is fine for the 2-view (stereo) case but a
known gap for modes with more than 2 views — those extra submitted views currently
duplicate the right eye's pose/FOV. Structurally UE still *renders* all N views; only
the layer metadata for views > 1 is a placeholder.

## Shell / IPC path status (black)

Under the shell or `XRT_FORCE_MODE=ipc` / `DISPLAYXR_WORKSPACE_SESSION`, the
compositor switches from zero-copy direct render to a private-RT + cross-process raw
copy (`bUseCopyPath`, `DisplayXRCompositor.cpp:243-249`). On this path UE content
currently arrives **black** at the D3D11 service compositor. Two contributing causes,
both diagnosed:

1. **Swapchain shape.** UE submits a single-tiled BGRA `arraySize = 1` swapchain on
   UE's RHI device, which the D3D11 service can't read cross-process (the native cube
   works on a dedicated device; the Unity path works via a standard `arraySize = 2`
   array swapchain). The fix lives in the UE plugin — convert to a standard array
   swapchain.
2. **Alpha = 0.** UE's tonemapper writes opaque RGB with `alpha = 0`. OpenXR's OPAQUE
   blend mode means "ignore source alpha," but the service composites premultiplied,
   so `alpha = 0` multiplies content to black. A runtime-side mitigation
   (`force_atlas_opaque_alpha` for OPAQUE clients) exists on a WIP branch and is not
   yet on `main`.

This is a **secondary** path; the standalone in-process zero-copy path is the
working, validated one. Tracking is in `displayxr-unreal` (branch
`fix/d3d12-ipc-workspace-sync-fence`) and the runtime-side WIP branch.
