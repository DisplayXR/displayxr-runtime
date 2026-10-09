# ADR-050: On macOS the runtime owns window placement — phase-snapped, atomic drag and resize by default

**Status:** Accepted (2026-10-08) · Metal in-process sessions implemented · extends
[ADR-033](ADR-033-placement-reports-geometry-weaver-owns-phase.md) and
[ADR-047](ADR-047-multi-screen-segments-and-per-screen-views.md) ·
spec: [XR_DXR_cocoa_window_binding.md](../specs/extensions/XR_DXR_cocoa_window_binding.md) v8 ·
code: `src/xrt/compositor/metal/comp_metal_placement.{h,m}`

## In one paragraph

A lenticular weave is a function of where the window's pixels land on the panel. On macOS the
WindowServer performs a title-bar drag by itself: it slides the **last presented frame** to every
intermediate position without the app re-weaving it, so the 3D stutters for the whole drag; and
AppKit's live resize runs a tracking loop inside `-[NSApp sendEvent:]` that blocks the render
loop. Neither can be fixed from the display processor. So, for every eligible DisplayXR session,
**the runtime performs the gesture itself**: it makes the window non-movable for AppKit, takes the
mouse-downs that would start a drag or a resize, asks the display processor for the
phase-equivalent position of every step (`snap_window_rect`, anchored at the gesture start), and
applies the move / resize in the **same Core Animation transaction** that presents the frame woven
for it. This is the default; an app can opt out.

## Context

- **Measured, not theorised.** The recipe was developed and judged on the Odyssey panel with the
  Leia SR Metal weaver (LeiaSR `tests/macos/metal_weaver/mtl_weaver_window.mm`, drag f2528fb33,
  native-drag control 87b064bb4, resize a849a5c45 — "perfect" on drag, edge and corner resize).
  The control (`SR_NATIVE_DRAG=1`, here `DXR_MACOS_NATIVE_DRAG=1`) shows the WindowServer drag's
  stutter side by side.
- **ADR-033** split placement from phase: placement reports geometry, the weaver owns all phase.
  That split stays. What changes is *who moves the window*: on macOS nobody but the runtime can
  move it in lockstep with the present, because only the presenter can put the move and the
  frame in one transaction.
- **Windows already does this** inside the vendor DP (`WM_WINDOWPOSCHANGING` snapping, see
  `XR_DXR_win32_window_binding` §2.4); Linux X11 does it in the app via
  `xrWeaveSnapWindowRectDXR` (#1588). macOS has no message hook a DP could use, and an app
  would have to re-implement the whole recipe (and get every gotcha right) to match.

## Decision

1. **Default: runtime-owned.** An in-process Metal session whose bound view is its window's
   `contentView` (`_handle`) or whose window the runtime created (`_hosted`) gets runtime-owned
   placement. Title-bar drag, edge / corner resize (8 pt zone on both sides of each edge), the zoom
   button and the title-bar double-click all go through it.
2. **Not eligible** (AppKit keeps the window, logged once): `DXR_MACOS_NATIVE_DRAG=1`; an app that
   chained `XrCocoaWindowPlacementInfoDXR` with `XR_COCOA_WINDOW_PLACEMENT_APP_OWNED_BIT_DXR`
   (spec v8); offscreen and shared-IOSurface (`_texture`) sessions — the app presents, not us; a
   workspace (shell) session (`DISPLAYXR_WORKSPACE_SESSION=1`); a bound view that is a sub-view
   (the app laid it out; moving the window is not ours to decide).
3. **DP contract.** A new Metal DP slot, appended after `set_present_origin` (ADR-020, no ABI
   bump, `XRT_DP_METAL_HAS_SNAP_WINDOW_RECT`):
   `bool snap_window_rect(xdp, origin_x, origin_y, target_x, target_y, *out_x, *out_y)` — the
   exact twin of the VK / D3D11 slot: backing (device) px, only the displacement from the
   gesture-start origin matters, a pure query. False (no slot, no lattice, no tracked viewer — SR's
   `SR_DECLINED`) means "use the target": the move is still atomic, only unsnapped.
4. **The runtime tells the DP the APPLIED origin.** Every frame of a runtime-placed session the
   compositor calls `set_present_origin` with the content origin the window actually landed at
   (read back after the move; AppKit may constrain a frame), relative to the screen holding it.
   **A runtime `set_present_origin` call wins** over anything the DP derives itself: the Leia
   plug-in's Metal DP currently polls its own present origin from the view
   (`update_present_origin` on `displayxr-leia-plugin` `feat/macos-metal-dp`); once it receives
   the runtime's call it must use that value for the frame (the polled one is the window's
   position *before* this frame's move commits). That change is the plug-in's, tracked separately.
5. **Multi-screen.** The snap uses the DP of the screen holding the content origin — the
   session's primary DP, or that screen's per-screen segment DP (ADR-047). The segment table for a
   frame is computed from the rect that frame is woven for.

## Mechanism (Metal)

- **Gesture** (main thread): `movable = NO`, `movableByWindowBackground = NO`; a local `NSEvent`
  monitor for left-mouse down / dragged / up returns nil to swallow what it handles. The mouse-down
  is hit-tested at the **event's** location (`convertPointToScreen:locationInWindow`), never
  `NSEvent.mouseLocation` — by the time a queued event is pumped the cursor may have left the edge
  zone. The traffic lights stay AppKit's. A drag step writes a pending content rect under a lock;
  the commit applies the latest one — one move per rendered frame.
- **Zoom** goes through a delegate **proxy** (`NSProxy`) installed as the window's delegate that
  implements `windowShouldZoom:toFrame:` (decline AppKit's animated zoom, queue the proposed frame)
  and `windowWillResize:toSize:` (tripwire) and forwards every other message to the app's own
  delegate. Chosen over swizzling (process-global, fragile under KVO's own isa-swizzle) and over
  notifications alone (there is no will-zoom notification). The app's delegate keeps receiving
  everything; the one visible difference is `window.delegate` returns the proxy (compare with
  `-isEqual:`, which the proxy forwards). If the app replaces its delegate later, the proxy
  re-wraps it on the next event.
- **Snap** (commit thread): drag / zoom = move semantics (origin snapped, size kept); resize by the
  left / top edge = the far edges stay fixed and the **size absorbs** the snap's 1–2 px
  correction; right / bottom-only resize needs no snap. Min content 320 × 200 pt. On Retina a
  window origin lands on whole points, i.e. `anchor + scale·Z` backing px: the runtime searches
  that reachable lattice (the vk_native X11 search, `u_x11_reachable_round`) and, within the
  nearest ring holding a phase-correct point, takes the one nearest the DP's own answer.
- **Present** (`comp_metal_compositor.m` layer commit): `presentsWithTransaction = YES`;
  `[CATransaction begin]` + `setDisableActions:YES` → apply the frame (`setFrameOrigin`, or
  `setFrame:display:NO` + `drawableSize`; never `display:YES`) → `nextDrawable` →
  `set_present_origin(applied)` → weave → `[cb commit]` → `waitUntilScheduled` →
  `[drawable present]` → `[CATransaction commit]`. The drawable's texture size is the authority
  for every downstream size.
- **App-initiated moves.** A frame change made outside this path (`setFrameOrigin` from the app, a
  display reconfiguration) is detected at the next present by comparing the window frame with the
  last frame the runtime applied; the new origin is snapped relative to the last **presented**
  origin and applied atomically. **Limit: the one frame the app's own move showed is off-phase
  (≤ 1 frame).** An app that moves its window continuously should opt out and snap its own moves
  (`xrWeaveSnapWindowRectDXR`, now routed in-process to the Metal DP).
- **Threading.** When `xrEndFrame` runs on the main thread (the test apps, hosted sessions, most
  AppKit apps) the move and the present are one transaction. Otherwise the drawable is sized on
  the commit thread, and the move + present are handed to the main queue and waited on for about
  one refresh (20 ms); on timeout the commit thread presents itself and the move lands when the
  main thread gets to it — non-atomic, ≤ 1 off-phase frame — **never a deadlock** (the main
  thread may be the one waiting on us).
- **Tripwires.** `windowWillResize:toSize:` during a live resize and
  `NSWindowWillStartLiveResizeNotification` count AppKit-native resizes (must stay 0); a
  post-present check compares the window's content origin with the origin the frame was woven for
  (`DXR_MACOS_PLACEMENT_TRACE=1` logs every step). Totals are logged once at teardown.

## Consequences

- Every DisplayXR Metal app on macOS gets stutter-free windowed 3D on drag and resize without a
  line of app code, and the vendor implements one pure query.
- The runtime is now in the window-management business on macOS. Full screen (green button) stays
  AppKit's: placement pauses while the window is full-screen or minimised.
- `sim_display` leaves the slot NULL (no lens lattice): unsnapped but atomic.
  `SIM_DISPLAY_METAL_SNAP_PERIOD=N` (N > 1) installs a test double that snaps horizontal
  displacement to whole multiples of N backing px, to exercise the snap, the size absorption and
  the Retina lattice without hardware.
- A window resized past the display the atlas was sized for no longer reads past the atlas (the
  crop blit is clamped; it used to assert under `MTL_DEBUG_LAYER=1`).

## Follow-ups (not in this ADR's implementation)

- **Vulkan and GL on macOS** (`comp_vk_native` over a CAMetalLayer, the GL compositor): same
  model, their own present paths.
- **IPC / service sessions on macOS** (`comp_multi_weave_macos.c`): the service presents, so the
  move must be carried to it — a different hand-off.
- **Leia plug-in**: implement `snap_window_rect` on the Metal DP via `srWeaverSnapToPhase(weaver,
  origin_x, origin_y, target_x, target_y, &x, &y)` (return false on `SR_DECLINED` / any non-success,
  `true` with the snapped point otherwise), guarded by `#ifdef XRT_DP_METAL_HAS_SNAP_WINDOW_RECT`;
  and let a runtime `set_present_origin` call win over its polled origin.
- Hardware judgement on the panel with the Leia DP (the runtime path was validated with
  sim-display only).
