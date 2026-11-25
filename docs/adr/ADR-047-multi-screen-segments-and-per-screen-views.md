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
   the default is on for in-process sessions on macOS, Windows and desktop Linux (#2305) and off
   on Android and in a host that feeds its own re-probe (the Windows / desktop-Linux service —
   `target_display_hotplug_set_host_feed`). On Windows and Linux the poll signature adds the
   platform outputs (`os_display_topology_read`: `EnumDisplayDevicesW` under a pinned
   per-monitor-v2 context; DRM connector `status` / `enabled`), and a changed signature must hold
   for two polls before it is an event (an HDMI blink costs nothing).

2. **The sweep, and copy-on-publish.** First, holding only the loader's lock: load every plug-in
   that is not a display-claim source yet (`target_plugin_rescan_claim_sources` — a plug-in that
   declined is probed again; sources that exist are never re-probed or released, because a live
   segment DP may have been made from their instance; the set stays in ProbeOrder; a
   PreferredPlugin pinning the active plug-in keeps the #1874 shortcut, loading nothing),
   re-enumerate the monitors and resolve every monitor's winner into a local registry. That half
   may wait on a vendor service describing a panel that just came up, so it runs outside the lock
   every screen-list reader takes (`g_display_info_mutex`). Then, under that lock: install the
   registry, re-resolve the panel's desktop rect and the per-screen preference, build the screen
   list, and compare it with the published one (`target_screens_compare`). Only a materially
   different list is **published** (a screen whose only difference is its EDID physical size is not:
   macOS refines that ~0.5–1 s after a plug-in, when IOKit publishes the EDID blob, and republishing
   for it rebuilt every live segment DP and re-acquired its eye tracker; a claim-level change or new
   bounds still publish): copied, and a **display-topology generation** bumped under its own small lock.
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
   place**: a segment DP whose screen is listed again with the same binding (EDID mm aside) and claim
   level, by the same plug-in instance, is kept (`comp_segments_dp_key_keeps` +
   `comp_segments_lifecycle_adopt` — a vendor weaver is not torn down because an
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

**Windows and desktop Linux (#2305).** On by default for in-process sessions, triggered by the
poll above (an in-process runtime owns no window or message loop to receive `WM_DISPLAYCHANGE` on;
the service keeps its own feeders). `target_plugin_rescan_claim_sources` now runs on Windows too.
Three stickiness rules keep a bound weaver safe: (a) the panel rect's **incumbent** monitor wins the
claim rule while the active plug-in still claims it; (b) when that claim vanishes (unplug, vendor
service restart, blink) the **last panel rect is kept** instead of falling to the origin / primary
rules, which could land on an unclaimed or virtual monitor; (c) every hotplug re-push to a D3D11 /
D3D12 / GL / Vulkan compositor is **re-anchored on the session DP's home screen**
(`u_screen_home_anchor`): those managers read "the session DP's screen" off SYSTEM_DEFAULT, so the
flag stays on the home screen, and when the home screen is gone or re-claimed no screen carries it —
segmentation pauses, never a second weaver on its panel. **Home gone → keep the list for a bounded
wait, then hand back to a ready screen** (addendum below). The managers still rebuild their segment
DPs on `set_screens` rather than carrying them over (Metal's session-DP identity logic is not ported;
follow-up). The service / IPC path: the service's own registry is rebuilt by the sweep, but the
generation is not forwarded to clients (macOS IPC sessions do not segment). Detail:
`docs/architecture/comp-segments.md` § *Display hotplug*.

### Amendment 8 addendum (2026-10-28): home gone — a bounded wait, then a ready screen (#2305)

Rule (c) as first written handed the window back to the home screen's DP before every rebuild. On
Windows hardware (a laptop with a DS1 panel on HDMI, 2026-10-27) an unplug went: Windows moved the
window to the laptop, 0.5 s later the laptop's segment DP took the window handle, then the unplug's
re-push rebuilt without the panel and first handed the window back to the home — that is, to a new
per-screen DP for the panel that had just left. That DP never became ready: it flat-blitted view 0
and reported no geometry, so locate fell back to the whole panel as the canvas, and the laptop
showed one eye of a 0.344 m frustum in a ~0.11 m window until the replug ("looked strange", "the
cube vanished"; no window-relative Kooima line for the whole unplug). Decision:

1. **A bounded wait.** While the home screen is gone, its plug-in wins no screen (unplugged, not
   re-identified or re-claimed), the compositor's last list still has the home, and every monitor
   left is where that list has it, the session leaves the compositor on its last list
   (`u_screen_home_keep_decide`, pure and unit-tested): the window stays with the laptop's segment
   DP and per-segment views, and a re-seat comes back with no rebuild in between. The wait is
   bounded at **60 s** (`U_SCREEN_HOME_KEEP_BOUND_NS`), the same window as the #266 move-back
   (displayxr-unity#266: the window goes back onto the panel when the same connector returns
   within ~60 s). The bound is checked every frame (`u_screen_home_poll_due`), so it expires
   without a new generation; a generation the expiry's own push picks up is still announced
   (`u_screen_home_poll_settle`), and a kept list the push cannot hand on (no in-process
   compositor took it) is dropped with one WARN instead of being retried every frame. A
   replug inside it re-pushes as usual; a monitor added, a moved layout, or the plug-in winning a
   screen again (the panel re-identified mid-replug) ends the wait early. Logs: `display hotplug:
   … gone, and '<plug-in>' wins no screen now (unplugged); the compositor keeps its last screen
   list for up to 60 s …`, then either `the kept screen list ends after N s — the home screen is
   back` or `screen 0x… ('<plug-in>') has been gone for N s (bound 60 s) — the compositor takes
   the list without it now …`.
2. **After the bound the home-gone rule applies**, as before: no screen carries the default and
   per-screen segments are off until the home returns.
3. **The hand-back goes to a ready screen, never to the departed one's DP.** Before a rebuild
   whose list has the home unplugged — the same condition as the wait: the home's id is not
   listed and its plug-in wins no screen — a manager that can move the window handle (D3D11,
   including the #918 hub; OpenGL; Vulkan on Windows) picks the target with
   `comp_segments_handback_decide` (pure, unit-tested): a listed screen whose DP is ready — it
   reports its screen's pixel and physical size, which the window metrics are built from. "Ready"
   means the geometry is known, not that the DP is weaving: a vendor DP whose weaver is still
   pending may report its attached panel's kept geometry (the Leia plug-in does,
   DisplayXR/displayxr-leia-plugin#471) and flat-blits until its weaver is up; the departed home's
   DP reports nothing, since its panel is not attached, so it is never a target —
   preferring the DP that already holds the window (nothing is re-made), else the ready screen the
   window covers most, whose window-holding DP is made while the old holder still holds the
   window. That DP moves into the session's primary slot and weaves the window on its own screen's
   geometry (on that rig, the laptop's sim-display DP; the window-relative Kooima lines come back);
   the departed home's DP is destroyed. **No DP is ever made for the departed home**: when the
   target's DP cannot be made, or no listed screen is ready, the window stays with the DP that
   holds it (a segment's moves into the slot), or with none, with one WARN. A home that is still
   there — re-claimed by another plug-in, or listed under a new position-derived id after a
   rearrangement — keeps the old hand-back to its own DP. The compositor remembers which screen the
   slot serves (`*_hwnd_hooks::away`, a `comp_segments_away`); when the home comes back, the first update of the
   new manager moves that DP back to its own screen's slot (still holding the window), gives the
   home a windowless DP, and the window follows the majority from there — the state a majority
   hand-off would have left. When the list has the home alone (the other screen off), the home's
   window-holding DP is made and takes the slot back.
3a. **While the home stays gone, a screen its plug-in wins again stands in for it** (#2305, after
   the Windows test of 2026-10-28). A serial-less panel's id is position-derived, so the panel
   alone at (0, 0) — the laptop's screen off — comes back under another id than the one it left
   with, and the session, whose home is still the old id, puts the default on no screen: before,
   the laptop's DP stayed in the slot and the panel showed the window flat until the layout went
   back. Now `comp_segments_away_decide` (pure, unit-tested) makes the one screen the home's
   plug-in wins the manager's primary, and the first update re-homes to it exactly as for the home
   itself: beside the slot's screen it gets a windowless DP and the window follows the majority;
   alone, it takes the window. The same when the slot's own screen leaves too (the OS primary
   screen, else the first listed screen that can make a DP). The record then names the stand-in
   instead of clearing, so the home's own DP, on its own binding, is made when the home returns;
   the session's home does not change. A re-home from a DP of the primary's own plug-in (the
   stand-in going home) frees the window first — a windowless home DP takes the slot, then the
   window-holding one — so one vendor never has two weavers on one window. Log: `segments: home
   screen 0x… is still gone and its plug-in '…' wins a screen again — '…' (screen 0x…) stands in
   for it: …`.
4. **Not covered:** the Vulkan manager on Linux and the D3D12 manager without the hub have no
   window-handle hand-off, so after the bound their session DP keeps weaving the window as before;
   Metal tracks its session DP's identity itself and is unchanged.

## Amendment 9 (2026-10-20): a screen's frame period and scanout adapter are its own

**Status: plan; phase A implemented for the D3D11 service (#2245; #2218 audit items 17 and 18).**

Decision 2 gave every segment its own DP, eyes and frustum, but two values are still decided once,
from the OS primary screen, and handed to every window and every segment: the **frame period** and
the **output (scanout) adapter** of the #918 split. On a hybrid box where the panel is scanned out
by one adapter at 60 Hz and a second monitor by the other adapter at a higher rate, a window wholly
on the second monitor is paced at the panel's period and woven and presented on the panel's
adapter, crossing adapters twice per frame (render → output adapter → the monitor's own adapter),
and DWM composes its present instead of flipping it (#2245 has the measurement).

### Today (main `5f81bdccd`; the quoted code is the anchor when lines drift)

*The period.*

- `compositor/util/comp_display_refresh_win.h:41-79` — `comp_display_refresh_hz_win(hwnd)` reads
  `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` for the monitor holding `hwnd`; `NULL` resolves
  `MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY)`.
- `d3d11_service/comp_d3d11_service.cpp:32478-32486` — the service calls it with `NULL` once, in
  `create_system`, into `sys->refresh_rate` ("Display refresh rate: … Hz"). Nothing re-reads it.
- That one number is the period of every client and every DP: the direct pipeline's
  `set_frame_timing` (`:15313-15314`), the compose path's (`:20187-20188`), the
  `CLIENT_TEXTURE` weave's (`:22635-22636`), the legacy per-client weave's (`:26554-26556`), the
  IPC commit backpressure's absolute deadline (`pace_period_ns`, `:26260`), and the predicted
  display period / time that `compositor_predict_frame` and `compositor_wait_frame` return to the
  client (`:9193`, `:9232`, `:9242`) — the horizon the client predicts its poses for.
- Segment DPs are never handed a period at all: `comp_d3d11_segments_record`
  (`d3d11/comp_d3d11_segments.cpp:775`) feeds them encoding, transparency, mode and phase, and
  leaves timing at the DP's default.
- The direct pipeline's render loop does **not** pace on the period: with an `APP_HWND` presenter
  it waits on that presenter's own frame-latency waitable (`capture_render_thread_func`,
  `:10597-10624`), so it follows whatever the presenting chain is allowed to flip at.
- In-process: `display_refresh_rate` is read once at compositor create from the window's monitor
  (`d3d11/comp_d3d11_compositor.cpp:5790-5796`, `d3d12/comp_d3d12_compositor.cpp:7088-7094`,
  `vk_native/comp_vk_native_compositor.c:13890-13895`), so a window created on the panel and then
  moved keeps the panel's period.

*The adapter.*

- `service_split_stage_a` (`comp_d3d11_service.cpp:2697`) resolves the scanout adapter with
  `getScanoutAdapter(panel_left, panel_top, …)` (`:2746`) where the origin is still the pre-DP
  `0,0`, i.e. the OS primary monitor — in its own words (`:2717-2727`), "If the service ever drives
  a non-primary panel this has to move". The verdict and the one output device are latched for the
  process (`sys->split_available`; `svc_out_device()` at `:2471-2474`); the ingest adapter is
  resolved from the same origin (`:32520-32533`).
- The service's segment managers are created on that output device (`pipeline_segments_weave`,
  `:14714` / `:14728`), so every segment DP — whatever adapter scans its screen out — weaves on the
  panel's adapter: Amendment 1's rule, "a segment DP lives on the device that presents", applied
  with one presenting device per process. The #918 egress ring (`sys->xbridge`, allocated
  worst-case at `:2854`) is likewise one ring, render → that adapter.
- The workspace's per-screen presenters (`svc_ws_presenter`, `:19197`, ADR-052) create their chain
  on the render device (`:19380`) and their DP through `create_dp_d3d11_for_screen` (`:19481`).
- In-process #918 Stage A resolves the scanout adapter from the session's display rect the same way
  (`d3d11/comp_d3d11_compositor.cpp:5408`, `d3d12/comp_d3d12_compositor.cpp:2164`,
  `vk_native/hub_d3d11/comp_hub_d3d11.cpp:1032`) and creates the segment managers on that output device
  (`comp_d3d11_compositor.cpp:2893`, `comp_d3d12_compositor.cpp:4038`,
  `hub_d3d11/comp_hub_d3d11.cpp:831`).

### Rule

1. **The frame period of a window is its screen's.** A window's period is the current refresh
   rate of the screen holding most of it (`comp_segments_majority`'s rule: largest area, an exact
   tie to the primary). A segment DP is handed its own screen's period; a per-screen presenter
   paces at its screen's. A window straddling two screens at two rates is paced at the majority
   screen's period; each segment DP still learns its own screen's period for its eye predictor.
2. **A screen's output adapter is the adapter that scans it out.** A segment DP and a per-screen
   presenter for a screen are created on that screen's scanout adapter — the monitor → adapter LUID
   mapping (`IDXGIOutput` / `DISPLAYCONFIG`) that Stage A already derives for the primary. A window
   wholly on a screen scanned out by the render adapter renders, weaves and presents there with no
   crossing; a straddling window pays a crossing only for the segment whose screen is on the other
   adapter.
3. **The primary screen is unchanged byte for byte.** A window wholly on the primary screen gets
   exactly today's period (the value read at create, not a re-read), device, egress ring and DP.
4. **Until a screen has its own output adapter, its own rate needs the chain's adapter.** A
   window (or segment) is paced at its screen's own rate only when that screen is scanned out by
   the adapter its presenting chain (or segment DP device) lives on — the monitor's adapter LUID
   from `QueryDisplayConfig`'s active path, compared with the device's. Otherwise it keeps the
   primary's period: the frame still crosses adapters and DWM composes it, so pacing it faster is
   wasted work. One WARN per transition says so ("screen 0x… is on another adapter — paced at
   the primary's … Hz until the per-screen output adapter (phase B)"). An unknown adapter on
   either side counts as "another". Phase B removes the gate's reason for most screens.
5. **Rates are read from the OS, cached, and re-read on a display change.** The cache is dropped on
   the service's display re-probe (`WM_DISPLAYCHANGE` → `ipc_server_request_display_reprobe` →
   `comp_d3d11_service_refresh_segment_screens`), which also covers a rate-only mode change.

### Phases

Order: A, then C's period half (the #2245 measurement is an in-process app), then B, then C's
adapter half.

- **Phase A — per-screen frame period (D3D11 service: direct pipeline and segment DPs).** The
  period handed to the session DP, the commit backpressure and the predicted display period follow
  the window's majority screen; each segment DP gets `set_frame_timing` with its own screen's
  period and an unknown residual (0, the DP's heuristic — the residual measured on the presenting
  chain describes the primary's scanout, not that screen's). A client that segments keeps the
  primary's period on the session DP, because under segmentation the session DP weaves only the
  primary screen's segment. Every one of these goes through rule 4's adapter gate
(`comp_segments_gated_refresh_hz`): on the measured hybrid box, where the service's output
device is the panel's adapter and the second monitor is on the other one, the window keeps the
primary's period until B. The render loop and the egress ring are untouched — the loop already
  paces on the presenting chain's waitable — so this phase is separable from B. *Risk:* low; the
  gate keeps a screen on another adapter at today's period, and a window whose rect cannot be read
  falls back to the primary's period, i.e. today.
- **Phase B — per-screen output adapter (service segment DPs and per-screen presenters).** The
  output side becomes per adapter: one output device, egress ring and fence set per scanout adapter
  in use, created on demand and released with the last segment on it; a segment manager keyed by
  (screen, adapter) creates each segment DP on its screen's adapter and crops from that adapter's
  egress slot; a segment on the render adapter crops the render-side atlas directly (no ring). The
  presenter for a window wholly on the render adapter's screen moves its chain to the render device.
  *Risk:* high — resource ownership moves: the atlas crop, the zones mask, the Local2D backdrop and
  the flat-2D unit each need a copy (or a source) on every adapter that weaves; the HWND hand-off
  (Amendment 2) now swaps a DP across devices; the latched `split_available` verdict becomes per
  adapter, and every `svc_assert_same_device(…, svc_out_device(sys))` site has to name which output
  device it means. Needs the two-adapter box for every step.
- **Phase C — the in-process compositors.** The same two rules for the in-process D3D11, D3D12 and
  Vulkan (split) compositors: the period re-evaluated from the window's majority screen when it
  moves, and Stage A's output device chosen per screen (B's per-adapter output side, in process).
  The measurement in #2245 is an in-process D3D11 app, so it is this phase, not A, that changes it.
  *Risk:* medium for the period (one value per compositor today, read in the DP handoff and the
  frame predictor), high for the adapter (each compositor's split owns its own bridge and repaint
  path).

### Out of scope

Linux and macOS (one adapter per desktop on the shipping configurations; their compositors read the
period from their own surface); a window straddling two adapters at two rates beyond rule 1's
"the majority screen's period" — it is not paced per segment; variable-refresh pacing; the workspace
compose path's presenter set (ADR-052), whose period follows once B gives each presenter its
screen; the `CLIENT_TEXTURE` and legacy per-client weaves, which keep the primary's period.

## Amendment 10 (2026-11-23): the view size follows the window's screen

**Status: accepted and implemented** (2026-11-24): every compositor (in-process Vulkan, D3D11,
D3D12, GL, Metal) and the out-of-process paths (the IPC shared-memory table, the desktop-Linux and
macOS weave engines, the D3D11 service). See `docs/architecture/comp-segments.md` and
`XR_DXR_display_info` v28. Companion to [ADR-066](ADR-066-live-display-driver-settings-envelope-sizing.md),
which makes display-driver settings apply while apps run. Roadmap: ADR-066 § *Roadmap*, stages 1
and 3.

Decisions 2 and 3 gave every screen of a window its own DP, eyes and frustum (#2757 does the same
for an off-panel window on in-process native Wayland). One value still comes from the session DP
alone: the **view size**. `xrEnumerateDisplayRenderingModesDXR` reports the head device's two modes
([ADR-065](ADR-065-two-runtime-rendering-modes.md)), so `viewScaleX/Y` is the session screen's,
whichever screen the window is on, and so is the tile an app renders (window × scale,
`docs/guides/displayxr-app-rules.md` INV-4.3). Suppose a window moves from a 3D panel, whose 3D mode
renders two half-resolution views, onto a laptop screen that could show full-resolution views. It
keeps the panel's half-resolution tiles. The per-screen display info carries one scale pair per
screen (`xrt_screen_info::recommended_view_scale_x/y`, `xrt/xrt_screen.h:97-98`) but never that
screen's mode table, and nothing tells an app that its screen changed.

### Principle

**The app owns its 2D/3D state.** Each screen's DP renders the part of the window on its screen in
the app's current mode. The runtime never switches the app's mode because the window moved. The lens
already works this way: a screen's lens wish is `session_3d && covered` (#2256,
`comp_segments_lens_wish`, `docs/specs/runtime/multiview-tiling.md` § *Multi-screen segments*).
Coverage decides whether a screen shows the window at all, and the app's mode decides how.

The per-DP 1:1 hold is kept. A DP that needs 1:1 pixels (it does not set
`XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE`; see `xrt_display_processor_vk_tolerates_resample`) goes
flat on a screen that resamples the window (#1595, #1654). A DP that tolerates resampling draws the
mode. Both are how a screen renders the app's mode; neither is a mode change.

### Decisions

1. **Swapchain sizing is the worst case over every claimed screen.** ADR-065 decision 4 sizes over
   the session DP's two modes. This amendment applies the same rule to every screen the registry has
   claimed at instance creation. The system atlas is the per-dimension maximum of tile count × view
   scale × screen pixel size, taken over every claimed screen's DP and every mode it declares
   (ADR-066 C1: the runtime keeps the full declared list and shows apps two slots of it), each mode
   against its own screen in its current orientation (`u_tiling_compute_system_atlas` over the union
   of the lists). `maxImageRect*` uses the same union (`oxr_system.c:509-576`; today it is the
   session screen's `display_pixel_width/height`), and so do the runtime's own worst-case buffers.

   ADR-010 stands as written: size once, never recreate. Each screen's 2D mode is its full pixel
   size, so the union is at least the per-dimension maximum of the screens' pixel sizes. The cost:

   | screens | union | RGBA8 per image | vs the session screen alone |
   |---|---|---|---|
   | ds1-linux: 3D panel 3840 × 2160 + laptop 2880 × 1800 | 3840 × 2160 | 33 MB | ×1.0 |
   | 4K 3840 × 2160 + super-ultrawide 5120 × 1440 | 5120 × 2160 | 44 MB | ×1.33 |
   | 4K landscape + 4K portrait (2160 × 3840) | 3840 × 3840 | 59 MB | ×1.78 |

   Apps typically hold three swapchain images, and the runtime's renderer atlases and egress ring
   follow the same size.
   - **Zero-copy.** On the Windows vendor path the one shipping zero-copy case is a full-screen 2D
     submission that exactly fills the swapchain (ADR-030). When another screen makes the union
     larger than the panel in either dimension, the panel's full-screen 2D frame no longer fills the
     swapchain, so it takes the crop copy, one blit per frame. `u_tiling_can_zero_copy()` stays the
     sole gate; nothing is special-cased.
   - **A plain 2D monitor counts.** A screen whose DP has only full-resolution one-view entries
     (sim_display's "Same as 2D" on an ordinary monitor) still counts toward the union. Otherwise a
     window moved there would be resampled up from the smaller size. The cost is the table above. A
     per-screen opt-out ("do not size for this screen") lets a user trade sharpness on that screen for
     memory. A window on an opted-out screen is resampled from the union. Its CLI and Dashboard form
     is an open question; it extends the contract in ADR-066 C1.3.
   - **Later screens.** A screen claimed **after** instance creation (hotplug, Amendment 8) whose
     modes fit inside the union joins with no change. One that does not fit, including a screen
     rotated after instance creation into a larger extent, takes ADR-066's recreation path (C2), and
     until the app answers, the DP resamples (C3).
2. **The per-frame view size depends on the screens the window covers.**
   - A window wholly on one screen uses that screen's DP's view size for the app's current mode:
     window px × that mode's scale.
   - A window straddling screens uses the **per-dimension maximum** over the screens it covers. Each
     segment DP crops its part and resamples it to its own view size. Cropping before the DP stays
     the law ([ADR-030](ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md)).
   - **Axes.** View scales are declared in each panel's own axes, but the maximum is taken in
     **window** axes. For a screen rotated by 90° or 270°, its `view_scale_x/y` are swapped into
     window axes before the maximum. The tile grid is the app's atlas layout and is not rotated. A
     screen's mode without `XRT_RENDERING_MODE_FLAG_CAN_ROTATE`, on a screen whose current orientation
     differs from the panel's native one, is shown flat on that screen, under the same rule as the
     1:1 hold. Whether every DP treats a rotated screen this way is to be checked in stage 1b.
   - **Different DPI.** Window px are the window's own pixels. Where the screens' scales differ
     (100 % next to 150 %), each segment is cut in that screen's pixel space (the #2791 Wayland
     defect is this class) and resampled to its own view size.

   This holds for one view set framed from the majority segment (a `PRIMARY_STEREO` session,
   decision 3, and every displayxr-common demo today, ADR-066 stage 1b) and for per-segment view sets.
   With per-segment sets every tile is the same size, so the atlas stays a uniform grid. For a
   **one-set** app, a straddling window is continuous across the seam **in position, not in
   sharpness**: its part on the screen with the smaller view scale is resampled, so a step in
   sharpness at the seam is expected. The 2D mode is window px × 1.0 on every display today, so in
   practice only the 3D mode's scale moves.
3. **The hysteresis is Amendment 2's.** The covered set that decision 2 reads changes only after the
   new set has held continuously for 0.5 s (`COMP_SEGMENTS_DEFAULT_HANDOFF_HOLD_NS`, the dwell that
   the HWND hand-off and the lens dead band already share). Dragging back and forth across a seam
   faster than that changes nothing. Some answers are majority decisions: for example, which
   screen's table `xrEnumerateDisplayRenderingModesDXR` describes for a straddling window
   (decision 4). Those also take the 20 % margin (`comp_segments_owner_*`). On top of that, the
   signal is coalesced to at most one per second (decision 4).
4. **The signal is ADR-066's `XrEventDataRenderingModeTableChangedDXR`.** When the effective view
   size changes because the window changed screens, the runtime does two things, in this order:
   1. It updates what `xrEnumerateDisplayRenderingModesDXR` reports for this session to describe the
      window's current screen: `viewScaleX/Y`, `viewWidthPixels/HeightPixels`, `tileColumns/Rows`,
      `modeName` and `hardwareDisplay3D`. For a straddling window, that is the majority screen with
      the per-dimension maximum scale.
   2. It queues the table-changed event, without the recreate bit, coalesced to at most one per
      session per second.

   This draft first proposed re-issuing `XrEventDataRenderingModeChangedDXR` with an unchanged index.
   That was rejected after review (ADR-066 C1.6 and § *Alternatives considered*): most consumers
   store only the index, Unity and the browser would pass a spurious same-mode event to scripts and
   pages, and an unsolicited mode-changed event could be read as the answer to a pending request.

   The live `recommendedImageRect*` (#1488, `oxr_views_change_*`) moves at the same time, so an app
   that enabled `XR_EXT_view_configuration_views_change` also gets that event, under the extension's
   own 1 Hz coalescing (`docs/specs/extensions/XR_EXT_view_configuration_views_change.md` §5).
   - **displayxr-common needs one `case`.** It already re-reads the table on the mode-changed event
     (`common/xr_session_common.cpp` ~558, since v2.34.0), and `ModeTable::refresh` counts a
     `viewScaleX/Y` change at the same index as a change (`common/mode_table.h:62-66`). The new event
     calls the same refresh. It must also size from ADR-066's worst-case statement: today it sizes
     from the session screen's `displayPixelWidth/Height` × the enumerated modes
     (`xr_session_common.cpp:270-300`), which a move to a larger screen would overflow. Both ship in
     the stage that ships this amendment. ADR-066 C1.6 lists every other consumer and what each needs.
   - **An app that ignores the event keeps the old size**, and the segment DP resamples what it is
     given. The picture is softer but never broken.
5. **The view count stays fixed per instance.** Moving a window never changes the
   `PRIMARY_MULTIVIEW_DXR` count or the number of views a session locates
   ([ADR-041](ADR-041-fixed-view-count-with-per-frame-activity.md)).
   - If a screen's 3D mode has fewer views than the session renders, that screen gets the first
     ones, and the inactive tail aliases as it does today.
   - If a screen's 3D mode needs more views than the session can fill, that screen shows the window
     in 2D. This is ADR-065 decision 6's floor, applied per screen. The app's own mode does not
     change, and no denial event is sent, because the app requested nothing.
   - **The clamp.** The located count is limited by `XRT_MAX_VIEWS` (8, `xrt/xrt_limits.h:27`) and
     the view-set capacity by `XRT_MAX_SEGMENTS` (2, `xrt_display_metrics.h:231`). The number of
     full per-segment view sets is `floor(XRT_MAX_VIEWS / max_view_count)`, at least 1 and at most
     the capacity. Segments beyond that number (a window covering three screens, or any straddle with
     an 8-view entry) are fed from the one-view-set path: the majority screen's set, cropped per
     segment (#2791). ADR-066 C1.5 has the examples.
6. **Kooima, legacy apps and zero-copy.**
   - **Eyes and framing are already per screen**: decision 2, the Amendment 4 addendum for zone
     elements, and #2757 for in-process native Wayland. This amendment changes only the size the
     views are rendered at.
   - **Legacy apps** (no `XR_DXR_display_info`) never see the table or the event. Their size stays
     the compromise computed at system fill-in, and each segment DP resamples it. The compromise
     comes from `oxr_system_fill_in`, `legacy_app_tile_scaling`,
     `oxr_legacy_compromise_mode_index` and #2641's adopted-window size. Whether it should be taken
     over decision 1's union instead of the session screen is open question 2.
   - **Zero-copy:** `u_tiling_can_zero_copy()` stays the sole gate (ADR-030). See decision 1 for the
     cost on the Windows vendor path. A segmented window already withholds zero-copy (#2223, #2228).
     No per-call-site proxy is added.
7. **`sim_display`'s default 3D mode is "Same as 2D"** (decided). sim_display declares one more
   entry: one full-screen view at `view_scale` 1.0 × 1.0, untracked, `hardware_display_3d` false,
   flagged `XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH` (ADR-066 C1.2). It is sim_display's default 3D
   mode, in slot 1 next to the full-screen 2D entry in slot 0. Most apps request 3D at start, and an
   anaglyph on a regular screen reads as broken. A laptop screen claimed by sim_display therefore
   stays sharp while the app is in 3D, and its lens state does not change. With ADR-066 Part B's
   viewer tracking on for that screen, the one view follows the viewer's head. The entry's name must
   not collide with #2576's `sbs_full`, which is a stereo-compositor output.

   The entry's atlas is the 2D size, so it does not grow decision 1's union. Worked straddle case
   on ds1-linux: a 3D app with its window across the 3D panel (two half-resolution views) and the
   laptop ("Same as 2D", one full view). The tile is the per-dimension maximum, which is full
   resolution. The session renders the panel's two views, and the laptop's segment DP shows the
   first one (decision 5). A one-view slot 1 that counts as 3D needs the change to ADR-065's trim
   rule that ADR-066 C1.2 spells out.

### What has to exist

- **A per-screen mode table.** For each claimed screen, the runtime needs that screen's declared
  mode list (view count, tiles, view scale, name, flags) from the plug-in that owns the screen, plus
  that screen's 3D mode (ADR-066 C1.2), which picks slot 1. Today a plug-in describes another
  screen only through `get_display_info_for_monitor` → `xrt_plugin_display_info`. That struct
  carries one `recommended_view_scale_x/y` pair, which is not a list. The list can be either an
  append under `struct_size` on that struct or a new optional slot (ADR-020 append-only, no ABI
  bump). If a plug-in answers neither, the runtime reads that screen's list as the head device's
  list, which is today's behaviour. This generalises #2218 item 14 (the default screen's metadata
  follows its per-screen DP) to every screen.
- **The effective table per session**, computed where the segment table is computed:
  - in-process, by the compositor's segment manager;
  - for an IPC client, by the service, which already sends the segment table by value
    (`compositor_get_segment_metrics`). The IPC client copies the head's modes once at connect
    (`ipc/client/ipc_client_hmd.c:362-366`). Its enumerate must answer from the effective table
    instead, refreshed together with the segment table.
- **The emit**, from the frame path next to the #1488 views-change edge
  (`oxr_session_frame_end.c`), and only on a real change. A frame where nothing changed must produce
  no write and no event (the LÖVR hazard, views-change spec §6).
- **ADR-066's worst-case statement**, in the same stage (decision 4).

### Open questions

1. **Which table a straddling window reports.** Decision 4 says the majority screen's table with the
   maximum scale. That mixes two screens in one row: one screen's name and `hardwareDisplay3D`,
   another screen's scale. The alternative is to report the maximum only in
   `viewWidthPixels/HeightPixels` and leave the scale as the majority screen's. This needs a review
   from the engine side.
2. **The legacy compromise.** Should it be taken over the union, or stay on the session screen with
   the DP resampling?
3. **A screen whose 3D mode needs more views than the instance** (decision 5). Is "2D on that
   screen" right? The alternative is to take the first N views and tell the DP to synthesise the
   rest. The runtime synthesises no pixels today.
4. **The per-screen sizing opt-out** (decision 1): how is it spelled in the CLI and the Dashboard,
   and is it per user or per machine?
5. **Rotation** (decision 2): does every DP flatten a non-`CAN_ROTATE` mode on a rotated screen, or
   do some weave in the rotated orientation correctly? Stage 1b's rotated-screen runs answer it.
