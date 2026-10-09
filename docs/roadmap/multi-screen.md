---
status: Active — joint DisplayXR × LeiaSR plan, approved 2026-10-07 (ADR-047)
owner: David Fattal
updated: 2026-10-07
issues: [69, 546, 793, 715, 46]
supersedes-in-part: [ADR-015 §3-§4 (one atlas split at the display boundary), multi-display-single-machine.md §"Phase 3 design decisions"]
code-paths: [src/xrt/targets/common/target_plugin_loader.c, src/xrt/targets/common/target_instance.c, src/xrt/state_trackers/oxr/oxr_session.c, src/xrt/compositor/vk_native/, src/xrt/compositor/util/comp_dp_factory.h, src/xrt/auxiliary/util/u_multi_display.h]
---

# Multi-screen DisplayXR — joint runtime × LeiaSR plan

> Written from a fresh read of the code on 2026-10-07 (runtime `main` at v2.27.1,
> leia-plugin v2.7.0, LeiaSR runtime 1.38 on `ds1-linux`). The SR-side column (§2 SR facts, §5) was
> written with the LeiaSR agent sessions; David approved the plan and the SR reservations on 2026-10-07.

## 1. Goal

A machine with N screens. Each screen is bound to its own display processor (DP), reports
its **own** display info and answers `xrLocateViews` with its **own** eye positions. A window
whose canvas covers several screens is rendered **per section** by the DP of the screen that
section sits on.

Concrete targets, in order:

| # | Setup | What the user sees |
|---|---|---|
| T1 | `ds1-linux`: laptop panel (eDP-1, 3456×2160 at +0+0) + Acer DS1 (HDMI-1, 3840×2160 at +3456+0), Wayland | A window straddling both: **anaglyph** on the laptop panel from sim_display's default eye, **woven 3D with DS1 eye tracking** on the DS1. |
| T2 | Two Leia panels, each with its own camera | Each half woven with *that* panel's tracking and *that* panel's geometry. |
| T3 | Windows and the service/hosted path | Same model under the shell and for hosted apps. |

Non-goals for this plan: networked displays (#70), a calibrated shared room frame for all
displays (ADR-008 / #46 — each screen gets its own DISPLAY space; a shared frame is a later
layer), Android.

## 2. What the code does today (read 2026-10-07)

Facts that drive the design. Anchors are `file:line` on `main`.

**Runtime**
- One active plug-in per process: `g_active_iface/instance` (`target_plugin_loader.c:57`).
  On Linux only the active plug-in is even loaded (`:3299-3307`); on Windows other plug-ins
  are loaded just to collect monitor claims (`:1256-1301`).
- `xrt_system_compositor_info` carries one display: dims, mm, origin, nominal viewer,
  eye-tracking modes, DP factories (`xrt_compositor.h:2830-3108`). It is copied by value over
  IPC (`proto.json:317`), feeds `XR_DXR_display_info` (`oxr_system.c:922-1003`) and the frozen
  view sizes (`oxr_system.c:363-403`).
- The per-monitor `dp_registry` from ADR-015 exists (`xrt_compositor.h:2743-2789`,
  `target_plugin_resolve_displays()` `target_plugin_loader.c:3311-3480`) but **is empty on
  Linux**: `os_display_edid_enumerate` is a stub off Windows (`os_display_edid_stubs.c:13-20`)
  even though RandR enumeration exists (`os_display_desktop_x11.c:259`). No compositor passes
  a real monitor id; VK/D3D11 in-process read the single factory fields directly
  (`oxr_session_gfx_vk_native.c:286-289`).
- One DP per compositor (`comp_vk_native_compositor.c:512`), canvas offset hard-wired to
  (0,0) ("one compositor instance weaves one window", `:5019`). The pure slice math
  `u_multi_display_compute_slices()` has unit tests and **no caller**.
- `xrLocateViews` takes eyes from the session's one DP
  (`oxr_session.c:321-365` → `vk_dp_predicted_eyes` `:3044`) and one `xrt_window_metrics`
  (one display rect, one window rect, `xrt_display_metrics.h:97-123`) into one Kooima
  frustum (`oxr_session.c:2763-2878`).
- Linux already has a **windowless weaver with an external phase origin** (ADR-033):
  `set_present_origin(window − display)` (`:11865`, pushed at `:6166` before each
  `process_atlas`). So ADR-015's primary/secondary HWND rule is a Windows-only concern.
- Shipped spanning behaviour on Linux (#1654): weave on the panel, flat 2D blit of view 0 on
  the off-panel bands; degrade to 2D when most of the window is off-panel (#1595).
- sim_display: anaglyph is the **default** output mode (`sim_display_device.c:57`,
  `SIM_DISPLAY_OUTPUT`); panel state is process-wide globals (`:57-69`); origin always (0,0);
  `probe_displays` claims every monitor at FALLBACK confidence; no `set_present_origin`.
- Services: one panel pipeline each (`comp_d3d11_service.cpp:1746,3762`; Linux comp_multi
  uses the one `info.dp_factory_vk`, `comp_multi_compositor.c:1989`). ADR-035's "one DP per
  panel" has no second-panel design.

**Leia plug-in (Linux, `src/drv_leia_linux/`)**
- One `srCreateInstance` per process (`leia_sr_linux_sdk.c:140-187`, `sr_ctx_ensure`);
  `SrDisplayCreateInfo.window = 0` = "primary SR display" (`:468`). One tracker
  (`g_ctx.tracker`), one lens. Eyes are polled per weaver via
  `srWeaverGetPredictedEyePositions` but every weaver shares the one tracker.
- EDID probe is first-match only and keeps no connector identity
  (`leia_edid_probe_linux.c:47,129,268`); `probe_displays` is **NULL** (`leia_plugin_linux.c:260`).
- Phase: `presentOrigin + viewportOffset` (`leia_sr_linux_sdk.c:1468-1485`). The weaver is
  windowless **only on native Wayland**: `leia_sr_linux_sdk.c:1470` passes `info->x11_window`,
  so on X11/XWayland the SDK builds a `Window2` and runs `decideWeave` every frame, including
  the **resampled-panel refusal** (`panelplacement.h:258-270`) that forces 2D when the panel's
  X11 size differs from scan-out (XWayland + fractional scaling, i.e. this box). Plug-in fix:
  pass `window = 0` always, since the origin is supplied per frame (ADR-033). With that one
  line, R2 is met on Linux on every window system.
- Windows (`src/drv_leia/`): SR context per DP, HWND into `srCreateDisplay`/weaver;
  no NULL-HWND handling; no `srWeaverSetPresentOrigin` call. `probe_displays` joins the
  runtime's monitor list with `srEnumerateDisplays` (leia-plugin#314, 2026-10-07): per-monitor
  FPC confidence + serial + opaque `displayId` kept in a plug-in-private binding table; older SR
  runtimes fall back to the frozen EDID table (every table-known panel VERIFIED, serial empty).

**SR SDK / SRService (facts from the LeiaSR session, Linux `ST-5525-linux-support-jul7`, Windows q3 line)**
- One SR instance sees exactly **one** SR display: `IDisplayManager::getPrimaryActiveSRDisplay()`
  (`modules/srDisplays/sr/world/display/display.h:302-322`). `SrDisplayCreateInfo.window`
  is documented as "the display containing this window" but `rt_CreateDisplay` ignores it and
  takes the primary (`sr_runtime_display.cpp:29`). No `srEnumerateDisplays`, no create-by-serial.
- Identity: SRService publishes an FPC serial; serial chars 8-9 = product code →
  `products/<code>/screen.ini`. Windows pairs monitor↔FPC by product code
  (`WindowsDeviceConfiguration.cpp:477-633`), primary = first device with both; all serials go
  to `Global\sharedDeviceSerialMemory` but clients read `serials[0]` only. Two identical panels
  are ambiguous (`display.cpp:695`). Linux SRService enumerates **no** monitors; client
  `getLocation()` matches DRM connectors by physical size with a 10 cm tolerance (LeiaSR#251),
  then RandR 1.5 only when units are provably device px → (0,0) under XWayland/Wayland (ST-5598).
- `srDisplayGetIdentifier` is an **opaque** uint64, not an EDID/connector key. System events
  carry no display identity and exist only on Windows.
- Eye tracking: one tracker, one camera, one lens **per service**, following the primary
  device (`eyetracker.cpp:73-95`); camera chosen via `calibration.yml`, not FPC serial
  (ST-3160 open). Eyes are in that display's frame (Linux fixed in LeiaSR#227). Lens
  preference and FPC controller are process statics → N trackers is an SRService refactor.
- Weaver: N instances per process work mechanically, but every one binds to the primary
  display and shares process-wide `SetDeviceSerialNumbers/SetGlobalParameters` → N copies of
  the same panel. Phase = `window_WeavingX + vpX`; `srWeaverSetPresentOrigin` (slot 64)
  replaces the window term on **both** lines. Windowless weaving works **only on Linux
  Vulkan** (`vkweaver.cpp:3008` passes `window == NULL` as the always-weave flag): on Windows
  the C99 path `rt_CreateWeaverDX11` → `new DX11WeaverBase(..., false)` always constructs with
  `constructedWithoutWindow=false` (`dx11weaver2.cpp:2383`; `dx11weaver.cpp` is the unused v1
  backend) and a null window makes `canWeaveInternal` return **false**
  (`WeaverBaseImpl.ipp:641-644`), Linux GL likewise (`glweaver2.cpp:1496`). Verified on q3
  `release-candidate/david-q3-2026` @ 2627fe01a by the Windows LeiaSR session. **So R2 on Windows
  and Linux GL is an SDK change** (folded into SR-P0), not a plug-in change.
- R1 not implemented: on Windows with an HWND the SDK still runs monitor (5 s) and FPC (1 s)
  polling, the `WM_WINDOWPOSCHANGING` snap, `getDrawRegions`, and a per-frame `canWeave`. On
  Linux (after LeiaSR#266) monitor polling is a no-op (the 5 s FPC loop still runs,
  `.ipp:530-595`, reloading correction textures only) and `decideWeave` weaves iff
  viewport∩client overlaps the panel (explicit origin path when `SetPresentOrigin` was called).
  Windows q3 has no `applicationOwnsPreference`: `updateLensHintState` (`WeaverBaseImpl.ipp:918-929`,
  called on every weave from `dx11weaver2.cpp:1414`) writes the lens and will fight a DP that
  owns the lens until SR-P0.
- `SrDisplayCreateInfo.window` is ignored by `rt_CreateDisplay`: the Windows plug-in's per-DP
  "SR display" is always the primary even with an HWND (`leia_sr_v2_common.cpp:340-357` is
  wrong about this) until SR-P2.
- No refresh getter (plug-in hard-codes 60 Hz, #184). No Panoramic View work exists in LeiaSR.
  Open SR tickets: ST-5480/5481 (routing flag + user stories), ST-5259 (multi SR monitors:
  Acer DS1/DS2 EDID has a unique serial at bytes 0x0C-0x0F, Samsung does not), ST-5521,
  ST-5598, ST-5650/5649 (local-3D region, multi-display scoped out in Q8).

## 3. The fresh perspective — three decisions the prior design never made

### D1. Screens are first-class runtime objects; several plug-ins are active at once
A **screen** = an OS monitor. The runtime builds a `screen registry` from the OS monitor
list (Win32 / RandR / Wayland outputs, including under XWayland) joined with every loaded
plug-in's `probe_displays()` claims. Each entry owns: stable id, desktop rect, native px,
mm, DPI scale, refresh, connector/serial, the **bound plug-in**, its DP factories, and the
**screen's own display info** (mm, nominal viewer, view scale, eye-tracking capability).
Unclaimed screens are bound to sim_display (mode per policy, default anaglyph, 2D optional).

This replaces "one active plug-in" with "one *pose-source* head device + N bound screens".
The head device stays single (it is the qwerty/rig pose source, not display geometry).
`xrt_system_compositor_info`'s display fields become a cached copy of screen 0 for
back-compat and stop being the source of truth.

### D2. Segments, not atlas slices
A window's canvas ∩ each screen rect = a **segment**. A segment has its own DP instance
(created from *that* screen's factory, with a screen binding), its own present origin, its
own view pair (eyes from *that* DP's tracker, or the screen's nominal viewer), and its own
Kooima frustum computed from the segment rect relative to *its* screen. The app renders
each segment's views; the compositor hands each DP its segment's tiles with
`canvas = segment rect` and composites the woven results into the one presented surface.

This **supersedes ADR-015 §3-§4** ("render one atlas, split it at the boundary"). Slicing one
atlas is only correct if both screens share eyes, size and pose; with a tracked DS1 next
to an untracked laptop panel it is geometrically wrong on one of them. The slice helper
(`u_multi_display_compute_slices`) survives as the segment-rect math.

On Linux every segment DP is windowless with an explicit origin (ADR-033), so there is no
primary/secondary role. On Windows the real HWND still goes to the majority segment's DP
for phase snapping; the others are windowless (SR ask R2-Windows).

### D3. Per-screen views on the existing multiview surface, with a per-view screen binding
No new view-configuration type. `PRIMARY_MULTIVIEW_DXR` already reports a max view count
and a per-frame `activeViewCount` (ADR-041). Multi-screen adds, in `XR_DXR_display_info`
v22:

- `xrEnumerateDisplaysDXR(system)` → `XrDisplayDXR[]` {id, `XrDisplayInfoDXR`, desktop
  rect, eye-tracking caps, vendor id, flags(tracked, primary)}; one DISPLAY reference space
  per display (`XrDisplaySpaceCreateInfoDXR`).
- `XrViewDisplayBindingsDXR`, chained on `XrViewState` at `xrLocateViews`: for each active
  view, `displayId` + the segment rect in window pixels. Views are contiguous per segment,
  ordered left-to-right by desktop position.
- Max view count = 2 × `maxSimultaneousScreens` (runtime cap 4 → 8; the common 2-screen
  case is 4). `activeViewCount` = Σ views over segments this frame (2 per 3D segment, 1 per
  2D segment).
- Swapchain per-view size stays worst-case (ADR-010); the segment's `imageRect` says what
  was rendered (ordinary OpenXR), and the compositor's crop-before-DP rule already handles it.

**Compatibility.** `PRIMARY_STEREO` apps (Unity's fixed stereo, every legacy app) keep 2
views: they come from the **majority segment**; the other segments get today's #1654
behaviour (2D blit of view 0). Nothing that works today changes until an app opts into
multiview + display_info v22. An app may also pin itself to one display
(`XrSessionDisplayBindingDXR`), which turns off segmentation for that session.

### Why not the alternatives
- *N XrSystems / N sessions*: OpenXR apps hold one session; a window is one swapchain set.
- *Display zones per screen* (ADR-027 reserved `process_zone_frame`): zones are app-declared
  regions; segments are runtime-computed and change on every drag. Internally a segment
  **is** implemented with the zone machinery (per-region swapchain tiles, per-region DP),
  but it is not exposed as a zone to the app.
- *One compositor per screen for app-owned windows*: a Wayland/X11/Win32 surface is one
  object; it must be presented once. One compositor, N segment DPs.

## 4. Milestones (each lands on `main` behind its own gate and is demoable on `ds1-linux`)

| M | Deliverable | Runtime work | Plug-in / SR work | Gate / demo |
|---|---|---|---|---|
| **M0** Screen registry on Linux | `displayxr-cli displays` lists eDP-1→sim-display, HDMI-1→leia-sr with geometry + serial | Replace the Linux EDID stub with RandR/Wayland enumeration (`os_display_desktop_enumerate` already exists; XWayland path per #251/#1579: match by size+mm); load every manifest plug-in and collect claims on POSIX; registry becomes the screen registry (adds mm, viewer, et caps); `XRT_PREFERRED_PLUGIN_ID` becomes per-screen | leia-linux: `probe_displays` (match every connector, keep connector id); `serial` = FPC serial from the service's `active` device (one panel) until SR-P1 enumeration lands; EDID serial bytes 0x0C-0x0F as the Acer tie-breaker. **Done on both arms:** Linux leia-plugin#311 (runtime #1850), Windows leia-plugin#314 — on the two-panel `win` rig both panels claim VERIFIED with their FPC serials (AL id 0x1, DS1 id 0x2) once LeiaSR 1.38.0+2056 (D1-Win pairing, LeiaSR#448) is installed; on 2029 the DS1 was EDID-only. Mixed vendor sets need nothing extra: every registered plug-in is loaded for claims and the resolver picks per monitor by confidence, then ProbeOrder, with sim-display's FALLBACK claim as the floor | CLI output on this box; Control Panel #793 phase 1 reads the same table |
| **M1** Per-screen info API | `XR_DXR_display_info` v22: `xrEnumerateDisplaysDXR`, per-display DISPLAY space, session display binding; `cube_handle_vk_linux` prints both screens | oxr: back `XrDisplayInfoDXR` from registry entry; IPC: new `system_get_display_list` message instead of growing the by-value struct | none | header + `index.json` note + CTS-style selftest check |
| **M2** Segments, one vendor | A `cube_handle_vk_linux` window dragged across eDP↔DS1 is **anaglyph on both**, each half with its own canvas and origin, no 2D band | `comp_segments` helper (shared across backends): segment table from window metrics × registry, per-segment DP lifecycle with hysteresis, per-segment `set_present_origin`, per-segment `process_atlas` with `canvas = segment`, composite; sim_display gets per-instance state + `set_present_origin`; DP factory ABI gains a `screen binding` arg (ABI bump, ADR-020 append rule) | none | atlas capture shows two canvases; eyeball on panel |
| **M3** Per-segment views | Same window: `xrLocateViews` returns 4 views, each half rendered from its own frustum | oxr: per-segment Kooima + eyes from the segment DP; `XrViewDisplayBindingsDXR`; `activeViewCount`; tile routing by segment; legacy stereo = majority segment + 2D blit | none | cube renders the correct perspective on each half; `check_displayxr_app.py` rule for v22 |
| **M4 = T1** Mixed vendor on `ds1-linux` | **Leia weave + DS1 tracking on the DS1 half, sim anaglyph on the laptop half** | loader: two plug-ins resident, DPs created from each screen's factory; plug-in display info per screen | leia-linux: `window = 0` always (one line, `leia_sr_linux_sdk.c:1470`); per-DP display state instead of `g_ctx.display` / statics; `srLensDisable` right after `srCreateLens` if the lens must start off. Confirmed by the SR side: a windowless Linux Vulkan weaver fed the DS1 segment + present origin weaves exactly that viewport, votes the lens on once, then the DP owns it. Works on today's SDK for **one** Leia panel. With SR-P0 (verified on the DS1 2026-10-07, branch `ST-5481-weaver-external-routing`): check `SrWeaverRoutingCapabilities` (tag 24) chained on `SrRuntimeCapabilities.pNext`, then chain `SrWeaverRoutingInfo{mode=SR_WEAVER_ROUTING_EXTERNAL}` (tag 22) on `SrWeaverCreateInfoVulkan.pNext`, `window = 0`, present origin per frame, lens via `srLensEnable/Disable` | David eyeballs it; screenshot both halves |
| **M5 = T2** Two Leia panels | Each panel tracked by its own camera | per-screen tracker plumbing already in place from M3 | **SR-P1** enumeration + identity, **SR-P2** bind display/weaver/lens/tracker by `displayId`, **SR-P3** N trackers + lens per display (SRService refactor, ST-3160 camera pairing). Plug-in: per-DP SR display + tracker, claims from `srEnumerateDisplays` | needs a second panel on a dev box |
| **M6 = T3** Service / hosted / Windows | Shell and hosted apps on N screens; Windows parity | ADR-035 amendment: one presenter per 3D screen in the service, clients segmented across presenters; hosted window per screen (#715 fix folds in); Windows: HWND to majority segment DP, windowless secondaries | SR-P0 **already ships on q3**, so no SDK gate: plug-in Windows DP chains `SrWeaverRoutingInfo{EXTERNAL, KEEP_DRAG_SNAP}` on the HWND DP and `{EXTERNAL}` + NULL HWND on secondaries, forwards `set_present_origin` → `srWeaverSetPresentOrigin` in **device px relative to that SR display, rotation applied** (a DPI-virtualised origin weaves on the wrong lattice and looks plausible — see `docs/reference/dpi-awareness.md`). **In-process D3D11 half: runtime `comp_d3d11_segments` + two appended slots (`create_dp_d3d11_for_screen`, D3D11 `set_present_origin`, no ABI bump) + sim_display D3D11; plug-in: screen-bound D3D11 DP chains EXTERNAL (+KEEP_DRAG_SNAP with the HWND) + `SrDisplayBindingInfo`, per-frame `srWeaverSetPresentOrigin` when windowless, scissor = canvas, and pins `srWeaverSetSimulatedViewer` on a non-active display (lens left to the active panel until SR D3). the HWND follows the majority screen (ADR-047 Amendment 2: 20 % margin held 0.5 s, windowless replacement first, rollback on failure; rig-unverified); M3 per-segment views ported too (D3D11 mosaic routing, `get_segment_metrics` / `set_view_routing`), so a straddling window's second panel renders from its own viewer; works under the #918 weave-on-scanout split (ADR-039, the hybrid-box default — segment DPs on the output device, fed from the egress slot; ADR-047 Amendment 1), so no `DXR_D3D_FORCE_GPU` / `DXR_SPLIT_SAME_ADAPTER` knobs are needed. In-process D3D12 (the Unity provider's path): the same, pre-split — `comp_d3d12_segments`, appended `create_dp_d3d12_for_screen` + D3D12 `set_present_origin`, sim_display D3D12, D3D12 mosaic routing; rig-checked with the split off; single-DP under the #918 split for now. Service half: a direct (non-workspace) IPC client's window is segmented by the service's direct pipeline, with per-segment views over IPC (ADR-047 Amendment 3); compose/shell mode and hosted windows stay single-screen.** | shell with a window straddling two panels |

Ordering rationale: M0–M3 are hardware-free beyond "two monitors" and exercise everything
with sim_display, so the model is proven before any SR dependency. M4 is David's stated
target and needs **no SDK change** (SR-P0 improves it). M5 is the first milestone gated on
the SR SDK; SR-P0/P1 can land in parallel with M0–M3 so M5/M6 are not serialized behind them.

## 5. SR-side work items (proposed by the LeiaSR session; nothing lands until David approves)

| SR item | What | Unblocks | ABI shape |
|---|---|---|---|
| **P0** Weaver routing mode (= R1 + R2, ST-5480/5481) | Opt-in `SrWeaverRoutingInfo` pNext on every `SrWeaverCreateInfo*`: `SR_WEAVER_ROUTING_EXTERNAL` = null window accepted, no polling threads, no `canWeave` gate, one full-input 3D region, phase = present origin + viewport only, no lens vote (controller owns the lens), WndProc hook only with `KEEP_DRAG_SNAP` + a real window. Default stays SDK mode for non-DXR apps. **Merged on the Linux line (#432 → 72e131b24) and shipping on the Windows q3 line (`release-candidate/david-q3-2026` @ 120e46eaa) as of 2026-10-07.** Dev override without plug-in changes: env `SR_WEAVER_ROUTING=external|external,keep-drag-snap|sdk`. Linux: ~150 lines, no dispatch slot (`canWeaveInternal` early-return, `updateLensHintState` skip, GL factory accepts `window = 0`); Windows q3 twin: must land in the `*weaver2.cpp` v2 backends (dx9/10/11/12, gl, vk), not v1; one-region and phase = origin + viewport are already there (`WeaverBaseImpl.ipp:136-142`, `dx11weaver2.cpp:1685-1689`), so the diff is the `canWeaveInternal` early-return, skipping the two polling threads (`dx11weaver2.cpp:600-601`), `updateLensHintState` and `installCustomWindowProc`, plus pNext parsing × 6. **Caution:** the monitor thread also refreshes `SRMonitorRectangle`/orientation; in EXTERNAL the weaver reads them once at create and the runtime recreates it on topology change (P1 `DISPLAY_TOPOLOGY_CHANGED`). | M4 (wanted), M6 (required on Windows) | pNext + capability bit, zero slots |
| **P1** Display enumeration + identity (= R4) | `srEnumerateDisplays(instance, count, SrDisplayDescriptor*)`: `displayId`, FPC serial, product code, confidence (EDID_ONLY / FPC_VERIFIED), desktop location in device px or panel-relative + flag, native res, mm, refresh, orientation, platform key (EDID mfr/product + HMONITOR / DRM connector + RandR name). New event `DISPLAY_TOPOLOGY_CHANGED`. Linux SRService starts enumerating DRM; Windows exposes the whole device list. Fixes #251 / ST-5598. **Merged on the Linux line (#434 → 1190d252e) and shipping on q3 (Win slots 106/107; descriptor also carries device name + HMONITOR, desktop-global device-px rect) as of 2026-10-07.** Shape: `srEnumerateDisplays` slot 97, `srDisplayGetRefreshRate` slot 98, `SrDisplayDescriptor` SR_TYPE 25, `SR_INCOMPLETE` = 4, `SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED` = 18 (client-side 2 s DRM poll). Descriptor: `displayId` (== `srDisplayGetIdentifier`), product code, FPC serial when `confidence == FPC_VERIFIED`, location + `locationIsDesktopGlobal` flag, native px, cm, Hz, EDID vendor/product/serial, DRM connector (`HDMI-A-1`), RandR output name. Only monitors the SDK recognises are listed (eDP-1 is not). M0 plug-in recipe: two-call enumerate, join on EDID vendor/product/serial + connector, else size; keep `displayId` as the opaque key for P2; re-enumerate on event 18; refresh from slot 98 replaces the 60 Hz constant. `locationIsDesktopGlobal` is 0 under scaled XWayland (this box), so the runtime keeps its size-match placement fallback (#1579) for that case; `outputName` is set only when RandR is provably in device px. Try before merge: branch `ST-5481-display-enumeration` + `SR_RUNTIME_PATH=<build>/libLeiaSR_runtime.so`. | M0 (authoritative claims), M5 | slot 97 (Win 106) |
| **P2** Bind to a chosen display (= Tier 2) | `SrDisplayBindingInfo { displayId }` pNext on `SrDisplayCreateInfo`, `SrWeaverCreateInfo*`, `SrLensCreateInfo`, `SrEyeTrackerCreateInfo`. Weaver gets per-instance display, correction textures, size, orientation. Also either implement or correct `sr_display.h:125-130`, which still claims `window` selects the containing SR display. **Merged on the Linux line (#436, 2026-10-07; line head 876620d62 carries phases A–C); arrives on q3 with the Linux→q3 merge LeiaSR#440 (real Windows resolver included).** Shape: `SrDisplayBindingInfo{displayId}` SR_TYPE 23 on `SrDisplayCreateInfo` / any weaver create-info / `SrLensCreateInfo` / `SrEyeTrackerCreateInfo`; a bound weaver keeps that display for life. `SrDisplayBindingCapabilities` SR_TYPE 26 on `SrRuntimeCapabilities.pNext` reports `displayBinding` + `maxBoundDisplays` (1 until P3: only the FPC-verified display binds; EDID-only id → `SR_ERROR_DEVICE_NOT_AVAILABLE`, unknown → `SR_ERROR_DISPLAY_NOT_FOUND`). Plug-in: chain it together with `SrWeaverRoutingInfo` on the weaver create-info, any order. | M5 | pNext, zero slots |
| **P3** Per-display trackers and lens (= R3) | SRService refactor: N FPC controllers, N tracker/lens slots keyed by display, camera pairing via USB-hub topology (ST-3160). Needs two SR panels. | M5 | service-side |
| small | `srDisplayGetRefreshRate` (slot 98 / Win 107); until then the runtime paces from the OS mode | #184 | one slot |

Slot and tag reservations (97/98, Win 106/107, tags 22/23/24/25) were approved by David on
2026-10-07 and posted on #tmp-sr-dashboard. The SR session proposes the joint plan become the content of ST-5521
(George), with SR phases A–D = P0–P3 and this doc as the runtime half.

**Runtime answers to the SR session's questions (decisions, pending David):**
- (a) HWND case on Windows: in phase 1 the majority-segment DP keeps the SDK drag snap
  (`KEEP_DRAG_SNAP`); the runtime does not reimplement snapping. The lens is owned by the DP
  via `srLensEnable/Disable` on both platforms (already so on Linux). Moving snap into the
  runtime is a later simplification, not part of this plan.
- (b) Yes: the runtime pushes `set_present_origin` **per frame for every segment DP on every
  platform** (ADR-033), including Windows. EXTERNAL mode may be defined as
  "phase = present origin + viewport, nothing else". During a drag the origin is read from the
  window after the SDK snap, so the two agree.
- (c) The join key is **EDID mfr/product + desktop rect** (what `xrt_display_descriptor`
  already carries), with the platform key (DRM connector / RandR name / HMONITOR) as a
  tie-breaker. Under native Wayland the SDK has no output name, so the join must work from
  EDID mfr/product + size alone. The Acer EDID serial bytes (0x0C-0x0F) are read by the
  **runtime** from the EDID blob, not duplicated by the SDK; the SR "serial" is the FPC serial
  (`srLensGetSerialNumber`, slot 48, already works on Linux for the one-panel case). The opaque
  `displayId` is the handle the runtime passes back into P2; it never needs to parse it.
- (d) Phase 1 platforms: Linux (X11, XWayland, and native Wayland through the present-origin
  path, which is the default DisplayXR path on this box) + Windows. macOS was out of phase 1;
  its **monitor enumeration has since landed** (`os_display_edid_macos.c`: CoreGraphics
  displays joined to the IOKit EDID — the `EDID` property on Apple Silicon's DCP transport,
  `IODisplayEDID` on Intel — with the CG vendor/model/serial as the identity fallback). So the
  claim registry, `displayxr-cli displays --claims` and `xrEnumerateDisplaysDXR` list every
  Mac screen. Rects are top-down **points** (the space windows are placed in, as
  `XrDisplayDesktopInfoDXR` already was); the backing-pixel mode is the screen's
  `nativePixelWidth/Height`, and the display UUID is its device name and monitor-id key.
  **Metal segments** followed (the per-screen segment compositor, M2's Metal twin):
  `comp_metal_segments`, two appended slots (`create_dp_metal_for_screen`, Metal
  `set_present_origin` in backing px relative to the display's `CGDisplayBounds` origin — the
  units the Leia SR Metal weaver takes), sim_display Metal, and M3 per-segment views through the
  same state-tracker path (`docs/architecture/comp-segments.md` § *macOS / Metal*).
  On macOS a straddling window is only fully visible with "Displays have separate Spaces" OFF
  (the default is ON, which clips it to the display holding most of it; one-shot WARN, logout to change).

> Status surface: the per-screen / per-window state this plan creates is shown by the display dashboard (ADR-051, [`display-dashboard.md`](display-dashboard.md)): `displayxr-cli status`, the Control Panel Displays + Windows tabs, and the `get_screen_status` plug-in slot for the vendor cell.

## 6. Risks and open questions

1. **Render cost** of a straddling window is 2× views. Acceptable; the common case is one
   segment and pays nothing.
2. **Refresh / pacing**: eDP-1 and DS1 can differ (60 vs 120/165). The surface is presented
   once; the compositor paces to the segment holding the majority area. Phase on the other
   screen during a drag is covered by Wayland MOVE SYNC (#1748) / Windows snapping.
3. **Wayland geometry**: `org.displayxr.WindowGeometry` reports the content rect relative to
   the window's *own* monitor; segments need the full desktop rect → extension version bump.
   Fractional scaling per output must be checked per segment, not per panel (#1595).
4. **Tracker coordinate frame** per display (LeiaSR #227) must be display-centred for the
   per-segment Kooima to be right; a camera frame breaks M4.
5. **sim_display on a non-1:1 laptop panel**: anaglyph does not care about 1:1, interlaced
   would. Keep the 1:1 gate per segment and per DP capability.
6. **ABI**: screen binding on `create_dp_*` and per-instance sim state is an
   `XRT_PLUGIN_API_VERSION` bump; downstream-pins `abi` track repins leia automatically.
7. **Segment viewport/scissor is mandatory.** A windowless EXTERNAL weaver's default scissor is
   the panel rect: a backbuffer wider than the panel is clipped unless the caller sets
   viewport *and* scissor per segment. `comp_segments` must set both on every
   `process_atlas` (Vulkan `srWeaverSetViewportVulkan` + `srWeaverSetScissorRectVulkan`
   already exist in the Linux DP).
8. Who owns per-screen eye-tracking **mode** requests (`xrRequestEyeTrackingModeDXR`) when
   screens differ — proposal: the request carries an optional displayId; no id = all.

**Decided 2026-10-07 (David):** an untracked screen renders from its **own nominal viewer**; the
tracked screen's eyes are never translated onto it, because the relative pose between an untracked
and a tracked screen is not knowable (no shared frame, ADR-047). The visible seam discontinuity in
that configuration is accepted; it disappears when every screen is tracked.

## 7. Issue mapping

- #69 becomes the epic for M0–M6; #546 (ADR-015 3b) is **re-scoped** to M2 (segments) and
  its split-weave wording retired; #793 is M0's GUI; #715 folds into M6; #46 / ADR-008 stay
  deferred (per-screen DISPLAY spaces without a shared frame cover this plan).
- leia-plugin #43: R1 + R2 → SR-P0 (M6 on Windows; Linux Vulkan already windowless-capable,
  plug-in passes `window = 0`); R3 → SR-P3, R4 → SR-P1 (M5); **new Linux items** → M0/M4 (probe_displays, per-DP display state). #251 is
  closed by M0's runtime-side enumeration + SR-P1. #184 → SR refresh getter.
- LeiaSR: ST-5480 superseded by SR-P0; ST-5481 stories 1/2/3 = SR-P1/P0/(P2); ST-5259 → SR-P1/P2;
  ST-5598 → SR-P1; ST-3160 → SR-P3.
- New: ADR "Multi-screen: segments and per-screen views" (supersedes ADR-015 §3-§4),
  `XR_DXR_display_info` v22 spec section, `comp_segments` design note.
