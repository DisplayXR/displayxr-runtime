# Weave-service phase (R2) and the Windows woven-origin tag (R3): feasibility notes

> **Landed (2026-10-26):** R2 + R3 are on `main` as #2379 (`37cc70630`), first released in runtime
> v2.55.0. The Leia plug-in's L4 is displayxr-leia-plugin#450 (`36be25cb8`), shipping in v2.18.3.
> The notes below are the pre-implementation record and are kept as written. Where the code
> went, and the panel results: [ADR-060 §Implementation](../adr/ADR-060-texture-target-reference-window-binding.md#implementation-as-landed-2026-10-26).

> **Decided (2026-10-25).**
> - **R2 split rule:** option (a). The split predicate runs on every windowless present-owner
>   weave; the per-screen weave runs only when the window leaves the primary 3D screen.
> - **R3:** ships now as a diagnostic, after R2, on condition that it causes no regression.
>
> **First panel run (a Windows machine with a LeiaSR panel, LeiaSR 1.39.0+2958, 2026-10-25): both arms PASS** on
> `feat/r2-weave-windowless-phase` @ `cca967a92`:
> - **Build:** compiles clean on MSVC, including the service TU.
> - **Leia without L4:** "does not hint windowless phase", window-bound DP, no extra SR weaver,
>   no lens flip.
> - **sim-display:** "OWN WINDOWLESS display processor", segmenting on, phased from present
>   origin (316,200).
> - **Afterwards:** the machine was restored and hash-verified.
> - **Still to run:** phase on the panel (needs a seated viewer) and the Leia-with-L4 arm.

> **DRAFT, 2026-10-25.** These are companion notes to
> [ADR-060](../adr/ADR-060-texture-target-reference-window-binding.md), for the editor-viewport
> work's runtime items R2 and R3. They come from reading the code at `024e1bb66` and have not
> been run. "Not verified" items are listed at the end of each section.

## R2: the present-owner panel DP is bound to the client's HWND

### What happens today
- `comp_d3d11_service_weave_bind_window` stores the client's HWND and flips the presenter to SELF
  (`comp_d3d11_service.cpp:30118-30125`). The DP is reached through the **plain** factory
  `comp_dp_factory_for_window(..., COMP_DP_PRIMARY_MONITOR, D3D11)`, from two places:
  - the render thread's SELF branch → `pipeline_bind_panel_dp(present_hwnd = weave_hwnd,
    client_presents = true)` (`:15254, :15394`), which re-points a live DP through `set_window`
    (`:12757`) or creates one (`:12828`);
  - the #918 ingest DP in `svc_ensure_client_weave_dp` (`:5672, :5733`).
- The plain factory is not screen-bound, so the Leia plan is empty: SDK routing, no binding
  (`leia_sr_multi_win.h:87-89`). SR's `installCustomWindowProc` then calls
  `SetWindowLongPtr(GetAncestor(hwnd, GA_ROOT), GWLP_WNDPROC, …)` on the **browser process's**
  root window (LeiaSR `WeaverBaseImpl.ipp:522-534`).

### What fails, and what doesn't
- A cross-process `GWLP_WNDPROC` subclass is refused (`ERROR_ACCESS_DENIED`). SR **does not
  check the return**: it records `originalWndProc = nullptr`, keeps the foreign root in
  `g_windowObjectMap` (`:531-537`), and skips the restore at teardown (`:493-497`). There is no
  error, and the only log line is the info `installing wndProc handler`.
- **What is lost** is only the WndProc behaviour: the SDK drag snap (`WM_WINDOWPOSCHANGING`,
  `:279`), min/max enforcement (`:253`), and the move-flicker suppression (`:434-457`). The drag
  snap is already replaced by `xrWeaveSnapWindowRectDXR` (service `:32231-32290` →
  `srWeaverSnapToPhase` or the v1 in-process probe window, `leia_sr_d3d11.cpp:2557-2685`).
- **What works:** phase. SR re-reads `GetClientRect` + `ClientToScreen` every frame
  (`WeaverBaseImpl.ipp:86-96`), and both work cross-process. The Leia DP pins PMv2 around the
  weave (`leia_sr_d3d11.cpp:2531`).
- **Confirmed on a LeiaSR panel** (LeiaSR 1.39.0, 2026-10-25, T1–T3):
  - **SDK routing.** Both service weavers (the panel DP and the present-owner ingest DP on the
    browser HWND) are SDK-routed. SR's log shows `WeaverBaseImpl: installing wndProc handler`
    for the browser window, from the service process.
  - **Drag snap.** Four drags produced zero `#625 snap` lines; the only snap was at startup.
  - **SR's own occlusion gate.** SR ran its own occlusion verdict on the bound window
    (`Process chrome.exe(31392) is occluding this window`). That is another SDK-routing
    behaviour, and one that **can stop the weave**. EXTERNAL routing removes it. That is a
    point in favour of R2, and a behaviour change to state when R2 lands, because occlusion
    becomes the runtime's job (the present-owner's own visibility) rather than SR's.
- **Net:** no visible defect was observed, apart from the occlusion gate above, which can stop
  the weave. The real defects are a dangling foreign HWND in an
  SR global map, phase read in the weaver instead of reported by the placement authority
  (ADR-033), and no exact origin to tag the handback with, which blocks R3.

### Proposal
1. Give the present-owner a **windowless** primary DP: `create_dp_d3d11_for_screen(binding,
   window = NULL)`, which `xrt_plugin.h:1475-1477` already allows. With SR external routing, the
   Leia plan sets `routing_external` + `needs_present_origin` (`leia_sr_multi_win.h:90, 129-131`)
   with no plug-in change.
2. In `comp_d3d11_service_weave_submit`, before each `process_atlas` (`:31758, :31832, :31989`):
   `svc_seg_window_rect(weave_hwnd)` (`:14908`) − the primary screen's desktop origin →
   `set_present_origin`. The service process is PMv2 aware through its manifest.
3. **The gate is the same one ADR-060 adds:** `XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN`. A
   NULL-window create **succeeds** on an SR without routing (and on `DXR_LEIA_SR_API=v1`). The
   result is an SDK-routed windowless weaver that never reads the origin (`WeaverBaseImpl.ipp:801-803`),
   which gives the **wrong phase with no log**. So: bit clear ⟹ destroy it and keep today's
   HWND-bound DP.
4. Changes: `svc_ensure_client_weave_dp`, the SELF branch (`:15354-15396`),
   `pipeline_bind_panel_dp` (its key is the HWND and it returns early on NULL, `:12639`; it must
   never `set_window` a windowless DP), and `comp_d3d11_service_weave_submit`. Key the DP as "the
   present-owner's windowless DP", not by HWND.

**Size:** S, about 120-200 lines in the runtime, plus about 20 in the plug-in to set the bit (L4,
shared with R1).

### Risks
- **EXTERNAL turns off SR's own 2D-region split and its lens vote** (`WeaverBaseImpl.ipp:170-176,
  793-795, 1342-1344`). An unsegmented present-owner window that is partly on a non-SR monitor
  would be interlaced there, where SDK routing shows that part in 2D today. So **the windowless
  path must evaluate the split predicate on every weave** (`weave_seg_update`, which today
  returns false at `:30650` unless the client opted into segments). The per-screen weave itself
  is needed only when the window touches a screen other than the primary. The other choice is to
  restrict windowless DPs to clients that submit segmented (v19). The same rule applies to R1,
  with its cost analysis (ADR-060 §Consequences). Lens: the Leia DP
  drives the active display's lens itself (`leia_sr_d3d11.cpp:747, 1324`). Verify that under the
  browser's 3D-hold logic (L3).
- On the segmented path, push one origin per DP per weave (the `primary_windowless` rule,
  `comp_d3d11_segments.cpp:1035-1041`). Never push two.

### Not verified
`ERROR_ACCESS_DENIED` was not reproduced. `srWeaverSnapToPhase` on an EXTERNAL windowless weaver
was not tested. I don't know whether Chrome binds its root or a child HWND (a child would also log
SR's "Weaving in a child window").

### Hardware test (a LeiaSR panel, LeiaSR 1.39.0 with `externalRouting=1`)
Browser present-owner, before and after:
1. a static woven page, then a capture (`displayxr_atlas_trigger.chrome`): phase identical;
2. a window drag through `xrWeaveSnapWindowRectDXR`: no crosstalk walk at rest;
3. a window half on a 2D monitor: that half flat (this is the EXTERNAL risk);
4. the plug-in log shows `externalRouting=1` and `needs_present_origin` for the present-owner DP.

**The phase oracle, no face needed (T1b, running as the pre-R2 baseline).**
- **Setup:** a WebXR page with the left view solid red and the right view solid blue. Move the
  window by +1x, +1y and +7x with `SetWindowPos`, and capture the woven handback after each move.
- **Pass:** a panel-locked weave shifts by exactly (−dx, −dy) in window coordinates.
- **The same oracle covers R1** on a texture-target session: move the target rect, or the
  reference window, instead.
- **The run needs a tracked viewer, held still.** It cannot run untracked: with nobody in
  front of the camera, the Leia DP shows the left view only, even when each view carries its own
  colour (observed 2026-10-25). So either a seated viewer holds still across the captures, or
  the Leia plug-in pins a dev-only simulated viewer (`SrSimulatedViewer`). The second option is
  a dev-only plug-in change. Either way the DP owns the viewer, and a viewer move
  shifts the pattern just like a phase error does. So check that the eye positions in the
  plug-in log match in every capture.
- **What each step tests.** +1 px tests the origin units: device px, and no DPI virtualization.
  +7x crosses at least one lenticular period, so it shows a wrong origin that a 1-px step could
  hide behind the pattern's periodicity.

## R3: woven-origin tag (`XR_DXR_weave` v12 §5e) on the Windows service

### State
- **Linux:** the origin is captured in the weave, under the engine lock (`comp_multi_weave_linux.c:2770-2786`),
  and committed with a serial (`:3247-3252`). The types are `xrt_weave_woven_origin`
  (`xrt_weave_dmabuf.h:101-110`) and the 40-byte wire POD `ipc_weave_woven_origin`
  (`ipc_protocol.h:1384-1395`).
- **On the wire only for `weave_submit_dmabuf`** (`proto.json:808`). `weave_submit`, the Windows
  handle path, has no origin field (`proto.json:746-760`). The state tracker always fills
  `XrWeaveOutputOriginDXR` from a zeroed local (`oxr_weave.c:846-848`), so on Windows it reads
  `valid = false, serial = 0`, as §5e allows.
- Windows drops the bound window geometry (`ipc_server_handler.c:7432-7445`). The service reads
  the HWND's client *size* only (`:31205-31212`), and the origin only on the segmented path.
- **No Windows consumer.** The browser reads it only in its Linux files (patches 0316/0317/0382).

### Proposal
1. Append `{"name":"origin","type":"struct ipc_weave_woven_origin"}` to `weave_submit`'s out list,
   and update the pin in `tests/tests_ipc_proto.py:505-511`. Appending is safe across the
   `u_git_tag`-gated client↔service pair.
2. `ipc_handle_weave_submit` (`ipc_server_handler.c:7545`) takes it from a new
   `comp_d3d11_service_weave_woven_origin()` getter, on the same pattern as `weave_segment_count`.
3. Client `comp_ipc_client_compositor_weave_submit` gets the out parameter, and `oxr_weave.c:159, 1199`
   pass it through.
4. The service snapshots `{valid = true, x, y, serial = ++weave_woven_serial}` from **the origin it
   pushed to `set_present_origin` (R2)**. `logical_*` stays false.
5. Update §5e "Where it is filled" and the header comment (`XR_DXR_weave.h:1058-1062`). That is
   probably v21, so callers can gate.

**Size:** S, about 60-90 lines plus about 15 of tests and docs.

**Sequencing: after R2.** Before R2 the weaver reads the window itself inside `process_atlas`, so
any value the service records is a separate, earlier read and can disagree during a drag. After
R2 it is exactly the value the weave used.

### Open question
There is no Windows consumer yet. DWM moves the window, and the browser snaps through
`xrWeaveSnapWindowRectDXR`. The first user would be the viewport host's generation/tag check
and a 1:1 placement probe. Is that enough to bump the spec now, or should
it land with the host?
