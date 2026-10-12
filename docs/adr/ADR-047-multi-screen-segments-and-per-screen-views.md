---
status: Accepted
date: 2026-10-07
source: "docs/roadmap/multi-screen.md, #69"
supersedes: "ADR-015 §3–§4 (per-compositor multi-DP over ONE atlas, split-weave at the display boundary)"
---
# ADR-047: Multi-screen — segments and per-screen views

## Context

ADR-015 made DisplayXR the owner of multi-display vendor routing and sketched the
mechanism: one compositor holds one display processor (DP) per display its window
overlaps, renders **one atlas**, splits it at the display boundary and hands each DP its
slice. That mechanism was never built (the registry it needs is empty on Linux, and
`u_multi_display_compute_slices` has no caller), and a fresh read in October 2026 found
two problems with it:

1. **One atlas is only correct when every screen shares the same eyes and geometry.**
   Each DP owns its own eye tracker and its own panel geometry (ADR-015 §6 says so
   itself). A tracked Leia panel next to an untracked laptop panel, or two Leia panels
   with two cameras, need two different Kooima frusta. Slicing one atlas puts the wrong
   perspective on one of them.
2. **The OpenXR surface was never addressed.** Every prior document keeps one
   `XrSystem`, one view count, one `xrLocateViews` per frame. Per-display eye streams
   stop inside each DP; nothing says how a second screen's eyes become app views.

Linux also changed the premises: the weaver is windowless and takes an explicit phase
origin per frame (ADR-033), so ADR-015 §7's primary/secondary HWND rule is a Windows
detail, not the model.

## Decision

1. **Screens are first-class runtime objects.** A screen is an OS monitor. The runtime
   keeps a screen registry built from the OS monitor list joined with every loaded
   plug-in's `probe_displays()` claims; each entry carries geometry, physical size,
   identity (EDID ids, connector, vendor serial), the bound plug-in and its DP
   factories, and the screen's own display info and eye-tracking capability. Several
   plug-ins are resident at once. One head device remains (it is the pose source, not
   display geometry); `xrt_system_compositor_info`'s display fields become a cached copy
   of screen 0 for compatibility.
2. **A spanning window is split into segments, not atlas slices.** A segment is the
   window's canvas intersected with one screen. Each segment has its own DP instance
   (created from that screen's factory with a screen binding), its own present origin,
   its own eye positions (that DP's tracker, or the screen's nominal viewer) and its
   own Kooima frustum computed from the segment rect relative to its screen. The app
   renders each segment's views; the compositor gives each DP its segment's tiles with
   `canvas = segment rect` and composites the woven results into the one presented
   surface. The slice math survives as segment-rect math. On Linux every segment DP is
   windowless with an explicit origin; on Windows the real HWND goes to the
   majority-area segment's DP only for drag phase-snapping.
3. **Per-screen views ride the existing multiview surface.** No new view-configuration
   type. `PRIMARY_MULTIVIEW_DXR` already advertises a maximum view count and a per-frame
   `activeViewCount` (ADR-041). `XR_DXR_display_info` gains display enumeration, a
   DISPLAY reference space per display, an optional session→display binding, and a
   per-view display binding chained on `XrViewState` (view → display id + segment rect
   in window pixels). Views are contiguous per segment. `PRIMARY_STEREO` apps keep two
   views from the majority segment and the shipped flat-2D treatment elsewhere
   (#1654), so nothing that works today changes until an app opts in.

## Consequences

- ADR-015 §1–§2 (probe + registry), §5 (DP lifecycle), §6 (per-DP eye tracking) and §8
  (vendor-owned phase snapping) stand. §3–§4 are superseded by Decision 2; §7 is
  narrowed to Windows drag snapping.
- A spanning window costs one view pair per 3D segment. The single-segment case, which
  is every window today, pays nothing.
- The DP factory ABI gains a screen binding (ABI bump, ADR-020 append rule); sim_display
  moves its panel state from process globals to per-instance state.
- The vendor side needs, in order: an opt-in external-routing weaver mode (null window,
  always weave, phase = origin + viewport, no lens vote), display enumeration with
  identity, binding display/weaver/lens/tracker to a display id, and N trackers per
  service. The first three shipped on the LeiaSR Linux line on 2026-10-07; the fourth
  needs two panels.
- Milestones, gates and the SR work items: `docs/roadmap/multi-screen.md`.

## Amendment 1 (2026-10-09): segments under the weave-on-scanout split

The first Windows implementation refused to segment a window whenever the D3D11 #918
output-device split was engaged, which ADR-039 makes the default on every hybrid box and
keeps even on one adapter, so the shipping configuration never segmented. Decision: **a
segment DP lives on the device that presents.** Under the split that is the output
(scanout) device, where the session's own DP already is: the segment DPs are created there
through the unchanged `create_dp_d3d11_for_screen` slot (it is handed the output device and
context), they weave the output-side copy of the composed atlas (the egress slot), and each
segment keeps its own viewport, scissor and present origin exactly as off the split. The
split is not a separate segment path; the primary screen's DP keeps the real HWND. Detail:
`docs/architecture/comp-segments.md` § *Windows / D3D11*.

## Amendment 2 (2026-10-09): the HWND follows the majority segment

Decision 2's "the real HWND goes to the majority-area segment's DP" ships on Windows D3D11
(it first shipped with the HWND pinned to the primary screen's DP, David 2026-10-07). The
owner is the segment with the largest window-px area, by the same rule that frames a single
view set (`oxr_segment_views_majority` / `comp_segments_majority`). A hand-off is due only
when another screen beats the owner by at least 20 % of the window's on-screen area (a
60/40 dead band between two screens) continuously for 0.5 s, so a drag across a seam never
flaps it; a target that fails is not retried until the majority leaves it. Because a weaver
is one-per-HWND and a live DP's window is never re-pointed, the hand-off creates a
windowless replacement for the old owner (the primary screen always keeps a DP, as the
session's own), destroys the old owner's DP, then creates the new owner's DP with the HWND
through `create_dp_d3d11_for_screen` — rolling back if that fails — all between two weaves,
so a frame weaves with the old pair or the new pair and never with a gap. The windowless
primary takes its phase from `set_present_origin` like any segment; drag snap, the window
anchor and the hosted window's snap provider follow the owner. Eyes and view routing are
unchanged: the primary screen's DP still serves the primary screen. Logic:
`comp_segments_owner_*` (unit-tested); detail: `docs/architecture/comp-segments.md`.

## Amendment 3 (2026-10-09): segments on the service / IPC path

An `_ipc` client is composited by `displayxr-service`, not in its own process, so the
service has to segment its window and the client's `xrLocateViews` has to learn the table
from the other side of the wire. Decisions:

- **The service owns the screens.** The screen registry already lives in the service (its
  instance's `enumerate_displays`, which `xrEnumerateDisplaysDXR` reaches over IPC), so a
  client sends only its session's display pin (`compositor_segments_enable`); the service
  builds the client's segment manager from its own registry, on the device that presents.
- **Only the direct single-client pipeline segments** (`pipeline_default_policy_render`,
  an `APP_HWND` presenter). The workspace / compose path (`multi_compositor_render`) keeps
  one presenter, the service window on the panel, so it stays single-screen: that is a
  separate decision (per-screen presenters for the workspace), not a consequence of this one.
  It is ADR-052 (one presenter per 3D screen, one desktop-space layout).
- **The table and the routing cross the wire by value** (`compositor_get_segment_metrics`,
  `compositor_set_view_routing`, appended to `proto.json`); per-segment views are then
  framed client-side from each segment's eyes exactly as in-process, and the service paints
  the routed mosaic into the client's atlas. A client that never opts in — every
  single-screen box — sends nothing new and is composited byte for byte as before.

Detail: `docs/architecture/comp-segments.md` § *Service / IPC path*.

## Amendment 4 (2026-10-10): present-owner weave rects are segmented per screen

The DisplayXR Browser does not reach any of the sites above: it is a present-owner on the
`XR_DXR_weave` service path. It binds its HWND, submits pre-weave pixels plus window-relative
rects, and composites the woven handback itself, with the eyes flowing back out to it. That site
wove with the one session DP and returned one eye pair, so an inline-3D element straddling two
panels was woven on the primary screen's lattice and rendered from the primary screen's viewer on
both halves. Decisions:

- **`weave_submit` segments the bound window exactly as the direct pipeline does.** Same opt-in
  (`compositor_segments_enable`, the service's own screen list), same manager
  (`comp_d3d11_segments`), same table (`comp_segments_compute` over the window's client area
  with canvas = the whole window), same rules: each screen's DP weaves its segment with
  canvas, viewport and scissor = the segment and its own present origin; flat 2D where a DP
  cannot weave; the session's weave DP keeps the HWND and the primary screen. The manager is the
  present-owner's own, made on the device its woven handback lives on (`sys->device`, where the
  #1172 ingest DP is), the same rule as Amendment 1 (a segment DP lives where its target is).
- **Only the window-canvas layouts are segmented**: the v3 batch (window-sized SBS scratch) and
  the v6 N-view atlas. The legacy single-rect submit weaves one element-sized canvas per call and
  stays single-DP. A rect is split implicitly: every pixel of it is woven by the screen it lands
  on.
- **The 2D overlay is composited after the per-screen weave** by the runtime's blit, not handed to
  a DP: it is window-sized and no segment DP covers the whole window.
- **Eyes out per screen: an `XR_DXR_weave` revision (spec v19).** `XrWeaveOutputRectPartsDXR`,
  chained on `XrWeaveOutputDXR`, returns the window's segments (each with its screen's eyes in
  that screen's display space, its physical size and whether it is woven) and every submitted
  rect cut at the seams (`u_weave_rect_parts`: per part, the window rect, the rect-relative rect
  and the on-screen rect). The table crosses the wire by value in an appended call,
  `weave_get_segments` (the per-segment views' `xrt_segment_metrics`), fetched only when the
  caller chains the struct; `weave_submit`'s reply is unchanged. The client cuts the rects
  itself, so they cross the wire once. A caller that does not chain the struct, and every window
  on one screen, sees exactly the pre-v19 behaviour.
- **Drag snap follows the majority screen's lattice.** `xrWeaveSnapWindowRectDXR` asks the
  segment DP of the screen holding the majority of the window at the PROPOSED position, with
  Amendment 2's hysteresis (`comp_segments_owner_*`, 20 % margin held 0.5 s) kept separately
  from the handle owner because nothing is rebuilt; it falls back to the panel DP when that
  screen has no DP yet or its DP has no snap.

The browser's half (rendering a straddling rect's two parts from two viewers) lives in
`displayxr-browser-pvt`. Detail: `docs/architecture/comp-segments.md` § *Present-owners*;
contract: `docs/specs/extensions/XR_DXR_weave.md` §5g.

### Amendment 4 addendum (2026-10-10): an element's views come from its own screen

The browser renders no inline-3D pixels itself, so the per-screen eyes of `XrWeaveOutputRectPartsDXR`
have no consumer there: each element's views come from a zone-scoped `xrLocateViews` with
`XR_DXR_view_rig` (browser patch 0036), which the service frames in `ipc_try_get_sr_view_poses` from
the session DP's eyes and the window relative to the primary screen. An element on the second screen
therefore looked around from the primary screen's viewer and was framed against the wrong panel.
Decisions:

- **An element wholly on one non-primary screen is framed from that screen.** When the window the
  locating session frames against is woven per screen and the element's zone rect lies wholly on a
  non-primary segment (`u_weave_zone_screen_segment`: exactly one segment, table cut from the live
  window size, the screen's size known), the server swaps in the whole window relative to that
  screen (`u_weave_segment_window_metrics`, the same arithmetic as
  `oxr_segment_views_whole_window_metrics`) before the usual zone rebase, and uses that screen's
  eyes (predicted at the locate from its segment DP) and nominal viewer. The Kooima is then that
  screen's physical rectangle with the element where it sits on it; the rig math is unchanged.
- **A straddling element keeps the primary's views.** One pixel across the seam is enough. Its
  parts are still woven per screen; rendering it per part needs a second view set per element,
  which is a web API (displayxr-browser-pvt#272). An element on the primary screen, and every
  window on one screen, are framed exactly as before: no per-screen table exists for them, so the
  lookup answers before anything changes.
- **The table follows the window, not the session.** The browser locates from its browser-process
  session and weaves from its GPU-process session, both bound to its top-level window. The lookup
  (`comp_d3d11_service_get_zone_weave_screen`) takes the locating client's own last weave table,
  else that of any present-owner weaving the same HWND, published within the last 250 ms. It
  takes `clients_mutex` only; `render_mutex` is taken just for an element that is re-framed, to
  read that screen's fresh eyes.
- **A cheap split signal.** `weave_submit`'s reply gains a trailing `segment_count`, the count of
  the table that submit published (appended like the v12 fields; the git-tag gate refuses a skewed
  pair). The state tracker fetches the table (`weave_get_segments`) for a chained
  `XrWeaveOutputRectPartsDXR` only when it is non-zero, so a v19 caller pays no extra round trip
  for a window on one screen, which covers every window on a single-screen box.

No extension revision: the locate API is unchanged and the reply field is internal to the wire.
Detail: `docs/architecture/comp-segments.md` § *Present-owners*.

## Amendment 5 (2026-10-10): segments on the in-process OpenGL compositor (Windows)

The in-process GL compositor (`cube_handle_gl_win`, GL demos) carries the model as
`gl/comp_gl_segments`, a twin of the D3D11 manager — including the HWND following the majority
screen (Amendment 2) — through two more appended slots (`create_dp_gl_for_screen`, GL
`set_present_origin`; ABI stays 5). GL has no output-device split (ADR-037 §5), so Amendment 1 does
not arise. One decision is GL-specific: **a window is segmented only when the primary screen's
plug-in implements `create_dp_gl_for_screen`.** The primary segment is woven by the session's own
DP, and a GL DP built before this amendment re-states a whole-target viewport inside
`process_atlas`, so it would weave the primary segment's crop across the whole window; a plug-in
that implements the new slot promises to honour a non-empty canvas on its plain DP too. Detail:
`docs/architecture/comp-segments.md` § *Windows / OpenGL (M6)*.

## Amendment 6 (2026-10-10): the in-process Vulkan compositor on Windows

The Vulkan demos and `cube_handle_vk_win` reach the in-process Vulkan compositor, which segmented
only on desktop Linux (M2). It now segments on Windows too, and it does so on whichever device
weaves — so there are two arms, chosen by the Vulkan #918 output-device split (#1178), not by
this ADR:

- **Off the split** (render adapter = scanout adapter, the split's default on a single-GPU box):
  the Vulkan segment manager runs as on Linux, its DPs made by each screen's
  `create_dp_vk_for_screen`, with the window rect read from the HWND's client area in device px.
  Amendment 2 applies unchanged: the HWND follows the majority screen (same `comp_segments_owner`
  hysteresis, same windowless-replacement-first / rollback hand-off). The one Vulkan-specific
  rule: a DP a hand-off replaces is RETIRED, not destroyed, because a repaint parked on its fence
  with the compositor lock released may still execute a command buffer that references it; the
  retire list is drained before the new owner binds the window whenever no repaint is in flight,
  and otherwise the hand-off rolls back and waits for the majority to come back (no gap, no flap).
- **Under the split** (a hybrid box's default): the weave is a D3D11 one on the scanout adapter, so
  Amendment 1 applies as written even though the app renders Vulkan — the *D3D11* segment manager
  lives on the output device, its DPs come from `create_dp_d3d11_for_screen`, they weave the egress
  slot, and the hand-off swaps the split's own DP. The Vulkan manager is not used while the split
  is up; a split that retires mid-session hands the screen list back to it.

Per-segment views (M3) are unchanged on both arms: the Vulkan renderer paints the mosaic on the
app device (under the split it crosses the bridge like any atlas), `xrLocateViews` reads whichever
manager weaves, and `oxr_system` counts a registry Vulkan factory toward the view-set capacity on
Windows alongside D3D11 / D3D12. Both arms crop each segment where its views were painted (#1883):
the Vulkan manager from the renderer atlas's partition, the split from the woven egress slot's own,
recorded per bridge sequence. Detail: `docs/architecture/comp-segments.md` § *Windows / Vulkan*.

## Amendment 7 (2026-10-10): the session is tracked when any of its screens is

Decision 2 gave every segment its own DP and tracker, and the per-segment views already come
from each screen's eyes (a window on the second panel switched from nominal to tracked eyes when
the viewer sat in front of it). But every session-level tracking signal still came from the
primary screen's DP alone: `XrViewEyeTrackingStateDXR.isTracking` and its change event (what a
MANUAL app drops to 2D on), the `XR_DXR_weave` output's `eyesTracking`, the service's display
status, the workspace controller's FRAME_TICK viewer. On the two-panel rig: "it feels like only
the primary camera wakes the tracker" (David, 2026-10-10). Decisions:

- **The session's tracking boolean is the OR over the DPs that currently present for it**: its
  own DP, plus every live segment DP (in-process D3D11/D3D12/GL/VK through the segment table
  `xrLocateViews` already reads; the service's direct pipeline, table fresh within its publish
  window), every secondary workspace presenter of the workspace (ADR-052; see the rig addendum
  below for why not just those the tile's footprint touches), and every
  per-screen segment DP of a present-owner's weave (Amendment 4). Each screen counts once. Only
  the boolean changes: eyes, views and per-view raw blocks stay per screen.
- **Debounce on the combined signal, not a new one per DP** (`util/u_tracking_any.h`). Rises at
  once; with one DP presenting it falls at once (the pre-amendment pass-through, byte for byte,
  so a single-screen box and MANUAL's immediacy are unchanged); with several it falls only after
  the OR has been false for 0.5 s, so a viewer handed from one screen's tracker to the other's
  never flaps the session. The state tracker advances it on the locate that owns the session's
  side effects; the service advances one per client connection for the eye query and one for the
  weave submit. An IPC client passes the service's combined answer through (no second debounce).
  The system display status ORs undebounced (a read, not a decision).
- **FRAME_TICK viewer**: when the primary's DP sees nobody and a secondary presenter's DP sees a
  viewer, the controller gets that presenter's viewer mapped into the primary's display space by
  `comp_ws_map_pose` (desktop pixels at each screen's pitch, z unchanged). That mapping is the
  workspace's own one-layout model (ADR-052 D2): exact for coplanar screens, an approximation for
  angled ones, where it is still the best single frame the controller has.
- **Hook for per-display presence.** "Is a viewer present at this screen" is decided in one
  function, `u_tracking_dp_viewer_present()`, today the DP's eye validity. LeiaSR is landing a
  per-display user-presence event (`SR_EVENT_TYPE_DISPLAY_USER_PRESENCE`, display id +
  present/absent, capability-gated); when it ships, an append-only optional DP slot (ADR-020)
  carries it and that function prefers it when the DP answers. No SR SDK dependency now.

Not changed: the vendor's MANAGED auto 2D/3D stays per panel (each DP drives its own lens from
its own tracker); the in-process HUD shows the session DP's own eyes. Spec:
`docs/specs/vendor/eye-tracking-modes.md` § *Multi-screen*.

**Rig addendum (2026-10-11).** David, two panels, one cube tile on each, laptop camera blocked
with his face in front of the DS1: "the app collapses to 2D as if lost face". The logs showed:

- **No per-frame flicker.** Each `FRAME_TICK viewer: the primary sees nobody — publishing screen
  0xb72e…` was followed one frame (~17 ms) later by `back to the primary screen's viewer`, and
  only at a primary transition (00:18:53.847 / 00:19:33.822 / 00:21:09.758 on loss, 00:19:36.272
  on acquire), never in the seconds between. The primary is read first in a frame and the DS1
  second, so a presence change shared by both DPs that lands between the two reads shows as
  "primary lost, DS1 tracked" for one frame on loss and the reverse on acquire. The DS1
  presenter's own DP (a bound windowless LeiaSR weaver) reported nobody for the whole time the
  laptop camera was blocked: its tracked state follows LeiaSR's machine-wide user presence,
  which only the primary tracker drives today. That is vendor-side; the runtime cannot report a
  viewer no DP reports. The DS1 tile's session fell 0.5 s after each primary loss (the hold
  expiring), the laptop tile's at once.
- **Scope: every presenter of the workspace.** The laptop tile counted only the presenters its
  footprint touched, i.e. none, so it took the single-DP pass-through and fell immediately even
  had the DS1 seen him. The workspace is one experience: a viewer seen by any of its panels keeps
  every tile tracked. A placed tile of a multi-presenter workspace now ORs every secondary
  presenter (`comp_d3d11_service_get_other_screens_tracking`); falling then takes the 0.5 s hold.
  Eyes are unchanged, a tile's views stay its own screen's.
- **The FRAME_TICK viewer picks its source through a dwell** (`u_tracking_source_update`,
  100 ms): the primary whenever it sees a viewer, otherwise a secondary only once it has been the
  only one seeing a viewer for 100 ms, kept while it still does. A one-frame skew of a shared
  signal no longer flips the published viewer.
- **The app's 2D was not our event.** `cube_handle_d3d11_win` runs MANAGED and does not act on
  `isTracking`; the plug-in issued no lens switch in the window (`display mode switched` only at
  session start/stop). What looks 2D is the DS1 weaver's own MANAGED fallback (its eye pair
  collapsing, which is also how the plug-in derives `is_tracking`). Fixing that needs LeiaSR's
  per-display presence (`SR_EVENT_TYPE_DISPLAY_USER_PRESENCE`) or a per-device tracker that does
  not follow the primary's user-lost.


## Amendment 8 (2026-10-10): the claims are live — display hotplug

Decision 1 built the screen registry once, at instance create. A 3D panel plugged in while an app
ran therefore stayed unclaimed (or claimed by the fallback) until the app restarted, and an
unplugged one stayed listed. Contract rule 4 for a vendor integration says a panel plugged in must
light up within about 2 s, and an unplugged one must retract cleanly, with no app or service
restart. On the vendor side this is already live (the Leia SR macOS runtime re-authenticates the
panel on hotplug, reports it in `srEnumerateDisplays` once it is published, and raises
`SR_EVENT_TYPE_DISPLAY_TOPOLOGY_CHANGED`); a plug-in that **declined** at instance create, because
there was no panel then, was never asked again. Decisions:

1. **Trigger: the OS, a poll, and the plug-in — one debounced sweep.** A worker thread
   (`targets/common/target_display_hotplug`) runs the claim sweep when
   - the OS reports a display reconfiguration (`os_display_hotplug_subscribe`; macOS:
     `CGDisplayRegisterReconfigurationCallback`, whose callbacks arrive on the main run loop);
   - a **poll** of a cheap topology signature (the OS monitor list: names, rects, modes, scale)
     changes — every 500 ms, the safety net for a process whose main thread runs no run loop;
   - a **plug-in** asks: `xrt_plugin_host_iface::request_display_rescan(reason)`, carved out of the
     host `reserved[]` block like the Android slots (no ABI bump; announced by
     `XRT_PLUGIN_HOST_HAS_DISPLAY_RESCAN`; callable from any thread, even after the plug-in's
     `probe()` declined, since the function lives in the runtime). It closes the gap between the OS
     seeing the monitor and the vendor backend recognising the panel.

   Pacing (`target_hotplug_sched`, unit-tested): 300 ms of quiet after the last event, capped at
   1 s after the first, so a reconfiguration storm is one sweep. An OS or poll event also arms
   **follow-up sweeps** at +0.5, 1, 1.5, 2, 3, 4, 6 and 8 s, because a plug-in that cannot say when
   its backend is ready must still be re-probed after it is; a plug-in's own request arms none (it
   says it is ready). Vendor-neutral throughout. `DXR_DISPLAY_HOTPLUG=0|1` forces it off / on;
   the default is on where the OS notification is wired (macOS) and off elsewhere.

2. **The sweep, and copy-on-publish.** First, holding only the loader's lock: load every plug-in
   that is not a display-claim source yet (`target_plugin_rescan_claim_sources` — a plug-in that
   declined is probed again; sources that exist are never re-probed or released, because a live
   segment DP may have been made from their instance; the set stays in ProbeOrder; a
   PreferredPlugin pinning the active plug-in keeps the #1874 shortcut, loading nothing),
   re-enumerate the monitors and resolve every monitor's winner into a local registry. That half
   may wait on a vendor service describing a panel that just came up, so it runs outside the lock
   every screen-list reader takes (`g_display_info_mutex`). Then, under that lock: install the
   registry, re-resolve the panel's desktop rect and the per-screen preference, build the screen
   list, and compare it with the published one (`target_screens_compare`). Only a different list is
   **published**: copied, and a **display-topology generation** bumped under its own small lock.
   Nothing hands out pointers into the registry: a session takes one by-value snapshot
   (`xrt_instance::get_display_snapshot` — the list, the system info with its registry, and their
   generation, under the sweep's lock) and the compositors copy that. Every session reads the
   generation once per frame in `xrBeginFrame`, on the app's thread, and on a change re-pushes the
   snapshot through the existing `set_screens`, so the compositor's threading is unchanged: it
   re-tables on its next weave. The sweep is stopped from the system compositor's `destroy`
   (wrapped at create) before the info it rewrites is freed.

3. **Live sessions: the session's DP is never swapped; a re-claimed screen becomes a segment.**
   The session's own DP was made for one monitor by one plug-in, bound to the app's window, with
   the head device's mode table and the system's swapchain sizing built around it (ADR-045 D3: no
   live swap). It now **keeps weaving exactly that monitor while that plug-in still wins it**
   (`comp_segments_session_dp`, recorded from the first screen list a session sees). Every other
   screen — including that very monitor once a hotplugged vendor re-claims it (sim-display →
   leia-sr when the panel that sim-display was standing in for comes up) — is woven by a **segment
   DP from its own claimant, even as the only segment**: the single-segment split path that
   already served a window on a non-primary screen, enabled now also for one screen the session DP
   does not own. Swapping the primary DP instead would re-create a window-bound weaver mid-frame
   and change the display the head device describes; the segment path is already exercised on
   every backend, its DPs are windowless with an explicit present origin (exactly what the SR Metal
   weaver takes), and its lifecycle is hysteresis-gated. The cost is an idle session DP while the
   window sits on a re-claimed screen. A new screen table re-tables the live segment manager **in
   place**: a segment DP whose screen is listed again with the same binding, by the same plug-in
   instance, is kept (`comp_segments_lifecycle_adopt` — a vendor weaver is not torn down because an
   unrelated monitor came or went); every other one is destroyed on the commit thread between two
   weaves. **Unplug** is that case: the screen leaves the table, its DP is destroyed (the plug-in's
   `destroy` is where its weaver goes and its lens is switched off), and the window, which the OS
   has moved, is woven by whatever owns its new screen.

   Unchanged, by ADR-041: the view counts are fixed for the session, so a session that started on
   a one-screen system keeps one view set. A window wholly on a hotplugged panel is framed from
   that panel's eyes and geometry (the majority-segment single set); one straddling it gets the M2
   crop, not per-segment views, until the next session.

4. **Events: `XR_DXR_display_info` v23.** `XrEventDataDisplaysChangedDXR`
   (`1004999218`, carrying the new `displayCount`) is queued to each running session once per
   published change, after it has been applied. The spec already said apps should re-enumerate
   after a topology change but gave them no signal; nothing else is required of them.
   `XrSystemProperties`, the view configuration counts and the swapchain sizing do not change.

5. **The active plug-in does not change; rule 3 gets one carve-out.** The active plug-in (the head
   device, the rendering modes, eye tracking, the system's display info) stays what discovery chose
   — a new session picks the vendor up through ordinary discovery. But #1521 (the active plug-in
   wins every monitor it claims, at any confidence) would keep a hotplugged panel on sim-display,
   which claims every monitor as a FALLBACK. Minimum change: **the active plug-in's claim yields
   when it is a FALLBACK claim and a source ranked before it (lower ProbeOrder) claims the monitor
   above FALLBACK** (`TARGET_SCREEN_PICK_BETTER_RANKED`). Such a source cannot exist at instance
   create — a better-ranked plug-in that probed would be the active one — so nothing changes there;
   ProbeOrder forcing (`run_cts.ps1 -Plugin sim-display` registers sim ahead of the vendor), a
   PreferredPlugin, a `DXR_SCREEN_PLUGIN` pin and a per-screen preference all decide exactly as
   before (unit-tested, with mutants).

**Readiness, the window-bound weaver, and eyes (review by the LeiaSR macOS owner).**

- *A plug-in claims only what its backend vouches for.* SR lists a panel in `srEnumerateDisplays`
  as soon as its EDID matches (`EDID_ONLY`, no serial), before the service has authenticated the
  FPC; it turns `FPC_VERIFIED` (with the serial) only once `devices/<serial>/primary` is published.
  The Leia macOS plug-in treats `EDID_ONLY` as present-not-ready: no `probe()` bind and no
  `create_dp_metal_for_screen`, so no weaver and no lens touch the panel. It claims the monitor at
  the new **`XRT_DISPLAY_CLAIM_PRESENT_NOT_READY`** level (20, appended to the claim enum; announced
  by `XRT_PLUGIN_HAS_CLAIM_PRESENT_NOT_READY`): it outranks the fallback's claim (so sim-display's
  anaglyph never reaches a 3D panel), and the runtime flags the registry entry `not_ready` and
  clears its DP factories. Every compositor then shows that screen **flat 2D** (the existing
  "segment without a DP" path: one view, no weave, lens untouched), and a session DP made for that
  monitor does not weave it either (`COMP_SEG_SESSION_NOT_READY`). Vendor-neutral: any plug-in whose
  backend lags can use the level. `DXR_TEST_SCREEN_NOT_READY=<output|connector|id>` forces it for
  a test.
  While an `EDID_ONLY` entry is listed, the plug-in polls the enumeration every 500 ms,
  **open-ended** (the SR service retries a failed authentication with a 1 s → 30 s backoff, so no
  fixed window is long enough), and on the `EDID_ONLY → FPC_VERIFIED` edge calls
  `request_display_rescan` whether or not SR raises a topology event. The same race exists at
  instance create (a panel plugged just before the app starts) and gets the same answer: the
  plug-in declines, the poll runs, the verified edge starts a sweep. A request that arrives
  before the sweep thread exists (between discovery and system create) is latched and served by
  its first sweep. The runtime's own follow-ups now run to +32 s (0.5 … 8, 12, 16, 24, 32 s) for
  plug-ins that cannot signal. `DXR_LEIA_ACCEPT_EDID_ONLY=1` restores binding at `EDID_ONLY`.
- *Case D — a panel replugged under a live window-bound weaver.* The session DP keeps its screen
  only while the monitor id, the winning plug-in, the claim's **vendor serial**, the **desktop
  rect** and the **OS output handle** (macOS: the `CGDirectDisplayID`, carried as
  `xrt_dp_registry_entry::platform_id`) all equal what they were when it was made
  (`comp_segments_session_dp_screen`, with a reason: gone / re-claimed / re-bound). The same
  serial back at the same bounds under the same handle hands the screen back to the session DP —
  its weaver is kept, and the compositor re-sends the session's 2D/3D mode to it, since a
  replugged panel's lens comes up in its default state. A different serial, a renumbered
  `CGDirectDisplayID` or moved bounds is "re-bound": the panel gets a freshly created segment DP
  (a new weaver) and the old window-bound one stays idle for the rest of the session — it cannot
  be destroyed while it is the session DP (ADR-045 D3).
- *Eyes.* After a replug the tracker must re-acquire its camera; until then the DP reports
  nominal or last-good eyes, not tracking. The Leia macOS DP logs the edge (`eye tracking
  ACQUIRED (fresh_raw=1, non-nominal) N ms after …` / `eye tracking LOST`), so the time from a
  claim or regain to tracked eyes is measurable.

**Scope shipped.** macOS: the CoreGraphics trigger, the poll, the sweep, the Metal compositor
(Metal apps and Vulkan apps on the VK-via-Metal route, #1900). A Vulkan app that started on
sim-display takes the `vk_native` route (sim has a Vulkan factory), which does not segment on
macOS, so it does not pick up a hotplugged panel (force `DXR_MACOS_VK_VIA_METAL=1`, or start with
the panel attached). Test hook: `DXR_TEST_FAKE_MONITOR_FILE=<path>` — while the file holds a number
S the main display enumerates as two monitors cut at S points; holding `reid` re-identifies it (the
session DP's monitor "unplugged" with an unknown one in its place).

**TODO (other boxes).** Windows: `WM_DISPLAYCHANGE` on a message-only window as the trigger, and
the twin of `target_plugin_rescan_claim_sources` over the registry entries (the Windows claim-source
set is collected once and keeps declined plug-ins out); the D3D11 / D3D12 / GL / Vulkan managers
already re-table on `set_screens`, but rebuild their DPs rather than carrying them over. Linux:
RandR `RRScreenChangeNotify` (or a DRM `change` uevent) as the trigger; the POSIX rescan and the
Vulkan manager's `set_screens` are in place. Both: `DXR_DISPLAY_HOTPLUG=1` runs the poll, untested
there. The service / IPC path: the service's own registry is rebuilt by the sweep, but the
generation is not forwarded to clients (macOS IPC sessions do not segment). Detail:
`docs/architecture/comp-segments.md` § *Display hotplug*.
