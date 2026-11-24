---
status: Proposed
date: 2026-11-23
deciders: project lead
issues: [2766, 2218]
related: [ADR-010, ADR-020, ADR-022, ADR-028, ADR-030, ADR-041, ADR-047, ADR-051, ADR-054, ADR-065]
---
# ADR-066: Display-driver settings apply live; envelope sizing first, swapchain recreation as the rare path

**Status:** Proposed (2026-11-23). The project lead approved the direction; it is open for
cross-box review. Nothing here is implemented. **Merge gate:** no code for this ADR or for ADR-047
Amendment 10 merges to `main` before runtime v2.70.5 (the QA build) is tagged (§ *Roadmap*).

**What it changes in other ADRs:**

- It **supersedes the deferred "stage B"** of [ADR-065](ADR-065-two-runtime-rendering-modes.md)
  (§ *Deferred: changing the kind while apps run*). Stage B becomes cheap because the envelope is the
  mode list each driver already declares (Part C1). Recreation is kept only for a change outside
  that list (Part C2).
- It **amends ADR-065 decision 1**: the trim keeps the full declared list internally instead of
  discarding it, and slot 1 is 3D by position (C1.2).
- It **amends ADR-065 decisions 2 and 4**: the kind is chosen per screen by a user setting that
  applies live, and sizing is taken over the full declared list (C1.2, C1.5).
- It **amends ADR-065 decision 3**: sim_display's default 3D mode becomes "Same as 2D" (C1.2).
- It **amends ADR-065 decision 5** ("no swapchain is ever rebuilt"; C2).
- It **amends [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md)'s "allocate once, never
  recreate"**. Allocating once stays the rule, and covers every 3D-mode switch (C1). Recreation is
  allowed only on a table-changed event that carries the recreate bit (C2), which is sent only when a
  change falls outside the declared list.

It also records two related decisions:

- **Part B**: viewer tracking becomes a per-screen setting. This is the first per-screen setting
  that applies live.
- **§ Roadmap**: the order of work for this ADR and for
  [ADR-047 Amendment 10](ADR-047-multi-screen-segments-and-per-screen-views.md#amendment-10-2026-11-23-the-view-size-follows-the-windows-screen)
  (the view size follows the window's screen).

## In one paragraph

A display driver keeps declaring its full list of rendering modes, up to `XRT_MAX_RENDERING_MODES`,
as it did before ADR-065. That list **is the envelope**. The runtime keeps the whole list and sizes
everything fixed for the life of an instance over it, as it did before ADR-065: `PRIMARY_MULTIVIEW_DXR`'s
view count, `maxImageRect`, the size apps allocate, and the runtime's own buffers. Apps still see
exactly **two slots**: slot 0 is always the full-screen 2D entry, and slot 1 is the entry the
screen's **3D mode** setting maps to it. Switching the 3D mode in the Dashboard or the CLI only
**remaps slot 1** and sends a new `XR_DXR_display_info` event, **"the rendering-mode table
changed"**. There is no re-sizing, no recreation and no restart. The same event, with a recreate bit
set, is the rare path for a change outside the declared list (a new or updated driver). An app that
never answers either keeps working: what it submits is resampled, never corrupted.

## Context

ADR-065 fixed the run-time contract at two modes and made the 3D kind a start-up setting. A live
kind change was deferred because of four problems (ADR-065 § *Deferred*; #2766's review):

1. **Values fixed for an instance.** `PRIMARY_MULTIVIEW_DXR`'s view count and `maxImageRect*` are
   fixed at `xrCreateInstance`. Core OpenXR requires `xrEnumerateViewConfigurationViews` to return
   identical contents for the life of the instance. The Khronos carve-out,
   `XR_EXT_view_configuration_views_change`, may move only the `recommended*` fields
   (`docs/specs/extensions/XR_EXT_view_configuration_views_change.md` §3).
2. **No signal.** A kind change keeps index 1, so the mode-changed event (sent on an index change)
   says nothing.
3. **Copies taken once.** IPC clients copy the mode table once at connect
   (`ipc/client/ipc_client_hmd.c:362-366`). Drivers read their settings once per process.
4. **Apps never rebuild.** Most demos read the table once, and none rebuilds its main swapchain on a
   mode event (C1.6 has the per-consumer table).

#2766 also found that **a smaller atlas buys nothing**. The 2D mode (one view at full panel
resolution) sets the allocation on every shipping table. A 2-view half-resolution mode, a squeezed
side-by-side and a 2 × 2 quilt each fit inside it (`docs/specs/runtime/multiview-tiling.md`
§ *Worked examples*).

These two findings together are the point of this ADR. Before ADR-065 the runtime already sized the
swapchain and the view count over every declared mode. ADR-065 trimmed the list to two entries at
start-up, and that trim is what made a live change look expensive. If the runtime keeps the full
list for sizing and shows apps only two slots of it, problems 1, 3 and 4 do not arise for any mode
the driver declared. The allocation does not grow on any shipping table, and the view count is the
list's maximum, which ADR-041 already lets a session use only part of per frame. Problem 2 needs a
signal, and C1.6 explains why that is a new event rather than a re-use of the mode-changed event.

## Part B — viewer tracking is a per-screen setting

**The decision.** Each screen has a **"Viewer tracking"** setting with three values:
- **off**, the default: the camera stays closed;
- **in 3D only**: the camera is open while an app shown on that screen is in its 3D slot;
- **always**: the camera is open whenever an app is shown on that screen.

It is implemented by the existing viewer-tracking provider route (ADR-054 N6,
`docs/specs/runtime/tracking-provider-discovery.md`), with `sim-tracking` `SOURCE=webcam` (§8) as
the in-tree reference. It is **not** a `sim_display` output option. sim_display's `WEBCAM_TRACKING`
is a one-release deprecation alias (§8 *One core, two hosts*) and stays one.

B1. **The setting gates arbitration step 2 per screen.** The arbitration stays §4's, per screen
(§4b):
1. the DP's own eyes, if the DP tracks;
2. the best enabled provider, if the DP's viewer policy is `EXTERNAL_OK`;
3. the nominal viewer.

The setting decides whether, and when, step 2 is allowed on that screen. Today one provider serves
*every* `EXTERNAL_OK` screen that has no tracking DP (§4b). With the setting, it serves only the
screens where the setting is not off, and with "in 3D only" only while the app is in slot 1. A
tracking DP still wins its own screen, so the setting is not offered there (reason "this display
tracks the viewer itself"). A `FIXED` display is not offered either.

B2. **The setting is greyed out, with the reason, unless the device has a compatible camera and
estimator.** "Compatible" means a front camera associated with that screen, which the provider can
open and analyse. The provider probes, without opening the camera (as `sim_tracking`'s probe already
does, §8), and reports per screen either *available* or *unavailable* with a reason:
- no camera associated with this screen;
- no face estimator in this build (the default build compiles the stub estimator; §8 *Which
  estimator is built in*);
- no provider installed or enabled;
- the provider is unlicensed (§3 *Licence state*).

Today a declining provider logs one WARN and returns false from `probe()`. A structured reason
needs an appended, `struct_size`-gated slot on the tracking plug-in interface (ADR-020 rule;
`XRT_TRACKING_PLUGIN_IFACE_HAS_*` announcement). A provider without the slot reads as "available if
its probe succeeded, reason unknown".

B3. **Eyes are mapped into that screen's own space, and only that screen's sources drive its
views.**
- A provider's samples reach an enabled screen in that screen's display space, through the camera
  pose calibration (§6, keyed by screen). Today a provider reports in the primary screen's space and
  is translated onto the others (`target_viewer_map_offset`, §4b). Under B1 the provider is
  calibrated to the screen its camera belongs to.
- A **tracking DP's camera never drives another screen's views.** The 3D panel's tracker feeds the
  panel's views and weave, and nothing else. This is the bug class #2757 fixed for an off-panel
  in-process Wayland window, now stated as a rule. ADR-047 Amendment 7's session-level tracking
  boolean (an OR over screens) and its FRAME_TICK viewer mapping for the workspace controller are
  unchanged: they report state, they do not frame views.

B4. **Live for the views.** Changing the setting re-runs the per-screen arbitration, as a topology
publish does today (`target_instance.c::publish_screens_locked`, §4b *When it is evaluated*). The
views of a running app follow within a frame or so.
- The composed `XR_DXR_display_info` eye-tracking advertisement (§5) stays static per instance. An
  app told "no tracking" at instance creation can therefore receive tracked views. For a
  RENDER_READY app this is harmless. Whether to also queue `XrEventDataEyeTrackingStateChangedDXR` is
  an open question.
- When the setting changes the tracked flag of the active slot (Part C), the existing
  eye-tracking-state event is sent.

B5. **CLI, status and Dashboard: the contract agreed with the Dashboard owner.**
- **CLI.** `displayxr-cli tracking set --screen <key> off|3d|always` (`on` is an alias of `3d`),
  and `tracking reset --screen <key>|all`. The screen keys are the ones `dp use --screen` uses.
  Values are per user; `--machine` writes the machine store, and a machine lock wins, as for every
  component setting. The service learns of the write through the DIAG re-probe call that
  `dp use --screen` already makes, and through the component-policy watch for edits made elsewhere
  (`plugin-discovery.md` §4.2a).
- **Status (ADR-051).** Each screen row of `status --json` gains, next to the existing
  `eye_tracking.viewer_source / viewer_reason / viewer_policy / viewer_provider`:
  ```json
  "viewer_tracking": {"setting": "off|3d|always" | null, "effective": "off|3d|always",
                      "available": true, "reason": "…", "provider": "<id>" | null}
  ```
  `setting` is null when the user chose nothing. Keys are only added.
- **Dashboard** (`src/dashboard`, Displays page). Each screen card shows a three-way "Viewer
  tracking" control next to the existing "Calibrate camera…" action (`Pages/DisplaysPage.cs:303`),
  greyed out with the reason when it is unavailable. The Dashboard reads `status --watch --json` as
  today and writes through the CLI, so it stays a dumb client (ADR-051).
- **Migration.** Today `sim-tracking ENABLE=1` means "serve every `EXTERNAL_OK` screen". Its reading
  becomes "always, for every screen with no per-screen value", so existing setups keep working.

## Part C — stage B via the declared mode list

### C1. The declared list is the envelope (recommended design)

**C1.1 The driver declares its full list again.** A driver declares every rendering mode it can
show, up to `XRT_MAX_RENDERING_MODES` (8), through the existing `xrt_rendering_mode` array. That
was the rule before ADR-065, and the struct and ABI are unchanged. Each entry already carries what a
mode descriptor needs:
- atlas topology (`tile_columns` × `tile_rows`);
- `view_count`;
- `view_scale_x/y`;
- tracked or untracked (`XRT_RENDERING_MODE_FLAG_HAS_TRACKING`, ADR-022);
- `hardware_display_3d`, the lens state ([ADR-028](ADR-028-display-mode-recipe-vs-hardware-state.md));
- `mode_name`, which is also the label the Dashboard shows (B5, C1.3).

An entry whose `view_count` exceeds `XRT_MAX_VIEWS` (8, `xrt/xrt_limits.h:27`) cannot be presented.
It is dropped from the list at the apply point with one WARN. Raising `XRT_MAX_VIEWS` (for example
for a light-field driver) changes IPC and shared-memory layouts and is a separate decision.

**C1.2 Slots and the 3D mode, and what changes in ADR-065's trim.** Apps see two slots: the
enumerate returns two entries, with `modeIndex` 0 and 1, exactly as under ADR-065.
- **Slot 0 is always the full-screen 2D entry**, entry 0 of the declared list. It is not
  configurable.
- **Slot 1 is the screen's "3D mode"**: one entry from the declared list. "3D mode" is the
  user-facing name. This ADR also calls the slot pairing a *config* internally.

What changes in the trim:
- **The trim becomes a selection.** Today `u_rendering_modes_trim`
  (`auxiliary/util/u_rendering_modes.h`) keeps entry 0 and one 3D entry and discards the rest. It
  changes to keep the whole declared list in the device (internally, for sizing and remapping) and
  to present entry 0 plus the entry the 3D mode names. The `to_driver` / `from_driver` maps that
  ADR-065 already builds for a forwarded `XRT_DEVICE_PROPERTY_OUTPUT_MODE` request become that
  mapping: a request for slot 1 reaches the driver as "the entry slot 1 is mapped to", in the
  driver's own numbering.
- **Slot 1 is 3D by position.** `u_rendering_mode_is_3d()` (`u_rendering_modes.h:73-76`) defines "3D"
  by content: `hardware_display_3d || view_count >= 2`. "Same as 2D" puts a one-view entry in slot 1,
  which is neither. The change:
  - A presented table is classified by position: slot 1 is the 3D mode, whatever its content. A new
    helper, `u_rendering_modes_index_is_3d(modes, count, index)`, answers `index == 1` for a
    presented two-slot table.
  - `u_rendering_modes_toggle_target()` decides from the index: V goes to slot 0 from slot 1 and to
    slot 1 from slot 0. Today, from a flat one-view slot 1, it would compute "go to 3D" and stay
    there.
  - In the declared list, a driver marks a one-view entry that is meant to serve as a 3D slot (a
    passthrough "3D" mode) with a new `mode_flags` bit, `XRT_RENDERING_MODE_FLAG_3D_PASSTHROUGH`.
    It is the next free bit after `CAN_ROTATE` (`xrt/xrt_device.h:254-259`), append-only under
    ADR-022, with no struct change and no ABI bump. `u_rendering_mode_is_3d()` counts that bit as 3D,
    so the content test still classifies a declared list correctly. The bit does not set the lens:
    `hardware_display_3d` stays false and no lens switch is asked for.
  - Entry 0 of the declared list must still be flat, and the list must hold at least one 3D entry,
    or it is refused as today.
  - Every other site that infers 3D-ness from a mode's content must ask the index instead. This
    covers the key gates, the session-begin floor, the "3D mode" a legacy app is sized for
    (`oxr_legacy_compromise_mode_index`), and compositor checks of `view_count > 1` or `>= 2` that
    mean "the 3D mode". A sweep for these is part of the change.
- **Who picks the 3D mode.** It is a **runtime-owned per-screen setting**, stored like
  `dp use --screen` and keyed by the same screen key. Its values are the declared entries' names. The
  runtime owns the mapping, so a 3D-mode switch needs no new driver slot.
  - If the user has not chosen, the driver's own default applies. sim_display's `OUTPUT` keeps that
    role as a CLI setting that means "the 3D mode for a screen with no choice". It leaves the
    Dashboard's Components options block.
  - With neither, slot 1 is the entry ADR-065's trim keeps today (the one the device marks active at
    creation if it is 3D, else the first 3D entry). A driver that declares only two modes therefore
    behaves exactly as under ADR-065.

**sim_display (decided).** sim_display declares a "Same as 2D" entry: one full-screen view at
`view_scale` 1.0 × 1.0, untracked, `3D_PASSTHROUGH`. **It is sim_display's default 3D mode.** Most
apps request 3D at start, and an anaglyph on a regular screen reads as broken. Anaglyph and the
other kinds are explicit choices.

| 3D mode (label) | slot 1 | atlas vs the 2D size |
|---|---|---|
| **Same as 2D** (default) | 1 full-screen view, untracked, `3D_PASSTHROUGH`: the screen stays sharp while the app is in 3D | equal |
| Anaglyph | 2 views at half resolution (0.5 × 0.5, 2 × 1), tracked | half |
| Cropped SBS, Squeezed SBS, Interlaced, Blend | the existing kinds (`drivers/sim_display/sim_display_modes.h`) | at most equal |
| Quad | 2 × 2 quilt of 4 views (0.5 × 0.5) | equal |

- Slot 0 is the full-screen 2D entry in every case.
- **Quad stays out of the declared list unless it is selected at start-up** (interim decision). It
  is the only entry with more than two views, and leaving it out keeps sim_display's maximum view
  count at 2 (C1.8). Choosing Quad on a running system therefore reports `applies: restart_app`.
- The `OUTPUT` value names `2d` and `passthrough` currently mean "anaglyph, and start in 2D"
  (`sim_display_settings.c:51-54`). With "Same as 2D" as the default, they become aliases of it.
  The entry's own name must not reuse them, and must not collide with #2576's `sbs_full` (a
  stereo-compositor output).

**C1.3 The CLI, status and Dashboard contract for the 3D mode** (agreed with the Dashboard owner):
- **`displayxr-cli dp mode list [--screen <key>] [--json]`** lists each screen's choices.
- **`displayxr-cli dp mode set <name> --screen <key> [--machine]`** applies live, through the DIAG
  call. It is refused with a reason when it cannot apply (`applies` is not `now`).
- **`displayxr-cli dp mode reset --screen <key>|all [--machine]`** clears the choice. It is distinct
  from `dp reset --screen`, which resets the screen's display-processor preference.
- **`status --json`, per screen row:**
  ```json
  "mode_3d": {
    "active": "<name>",
    "setting": "<name>" | null,
    "source": "user|machine|driver_default|runtime_default",
    "choices": [{"name": "…", "label": "…", "views": 2, "tracked": true, "passthrough": false,
                 "applies": "now|restart_app|not_supported", "reason": "…"}]
  }
  ```
  `label` comes from the driver's declared entry (`mode_name`).
- **The runtime computes `applies` and `reason`.** The 3D mode is a runtime-owned choice, so no
  driver `get_setting_status` speaks for it (the "Not supported here" chip in the Dashboard's driver
  options comes from that slot, ADR-054 Amendment 3). The rule:
  - `now`: the entry is in the declared list of the screen's driver;
  - `restart_app`: the entry is declared only at start-up (sim_display's Quad today), or it needs
    more views than a running instance can locate (C3). The reason names which;
  - `not_supported`: the driver's `get_setting_status` for the setting that would declare the entry
    says so. Its note becomes the reason.

  A choice whose entry a running stereo session cannot fill is still `now`: that session is floored
  to 2D (C1.7), and the reason says so ("stereo apps show 2D").
- **Dashboard.**
  - Each screen card gets a "3D mode" picker, shown only when the screen's driver has more than one
    entry that can be slot 1.
  - The picker says "Applies now to running apps" and greys out a `restart_app` or `not_supported`
    choice with its reason.
  - The existing driver-options note, "Changes apply the next time a 3D app starts"
    (`src/dashboard/DisplayXR.Dashboard/Model/DriverOptions.cs:297`), stays under the Components
    options block, where it remains true for driver settings. It must not appear next to the 3D-mode
    picker.

**C1.4 What is the same as the pre-ADR-065 N-mode array, and what is new.**

| | Pre-ADR-065 N-mode array | ADR-065 | This ADR |
|---|---|---|---|
| What the driver declares | every mode, up to 8 | should declare two; a longer list is trimmed | every mode, up to 8 (as before ADR-065) |
| What the runtime keeps | every mode | two entries; the rest discarded | every mode, internally |
| Swapchain size and `maxImageRect*` | max over every mode | max over the two kept entries | **max over every mode** (as before ADR-065) |
| `PRIMARY_MULTIVIEW_DXR` view count | max over every mode | the kept 3D entry's | **max over every mode** (as before ADR-065), clamped (C1.5) |
| Who selects the 3D kind | apps (`xrRequestDisplayRenderingModeDXR` with any index) and digit keys 1–8 | a driver setting, read at start-up | **a user setting** (Dashboard / CLI), per screen, applied live |
| What apps see | N entries | two | **two slots** (as ADR-065); slot 0 fixed |
| Keys | per-platform maps, 1/2/3 | V toggles, 0 / 1 | V toggles, 0 / 1 (as ADR-065) |
| Changing the kind under a running app | an index change, with the mode-changed event | restart | **a slot-1 remap, with the table-changed event** (C1.6) |
| Per screen | no | no | **yes**: each screen has its own 3D mode (ADR-047 Amendment 10) |

So sizing and the view count go back to what they were before ADR-065. What is new is who selects
the kind, that apps see two slots, that the choice is per screen, and the change event.

**C1.5 What is sized over the full list.** The runtime takes the union over the declared lists of
all claimed screens' drivers (ADR-047 Amendment 10 decision 1, which has the worked cost):
- **The `PRIMARY_MULTIVIEW_DXR` view count.** It is the maximum `view_count` over the lists
  (`u_rendering_modes_max_view_count`) × the view-set capacity (ADR-047 M3), **clamped to
  `XRT_MAX_VIEWS` (8)**. Today `oxr_segment_views_multiview_count` (`oxr_segment_views.h`) caps the
  product without reducing the capacity, and the capacity is at most `XRT_MAX_SEGMENTS` (2,
  `xrt_display_metrics.h:231`). The rule:
  - The number of full per-segment view sets is `floor(XRT_MAX_VIEWS / max_view_count)`, at least 1
    and at most the capacity. The located count is that number × `max_view_count`.
  - A window covering more screens than there are full sets is fed from the one-view-set path for
    the extra segments: the majority screen's set, cropped per segment (#2791).
  - Examples: sim_display Quad (4 views) × 2 screens = 8 fits exactly. An 8-view light-field driver
    gets one set, so a straddling window is a one-set window.
- **`maxImageRect*`** and the live `recommendedImageRect*` ceiling.
- **The atlas apps allocate.** It needs an app-visible statement (C1.6).
- **The runtime's own worst-case buffers**: the renderer atlases (#2564,
  `comp_d3d11_renderer.h:380`, `comp_d3d12_renderer.h:521`), the #918 egress ring (`sys->xbridge`),
  the service-created swapchains of IPC clients, and the `_texture` shared surface (ADR-010).

`u_tiling_compute_system_atlas` already takes its maximum over whatever array it is given; it is
handed the full list again instead of the two presented slots.

**The vendor plug-in (decided).** The vendor plug-in declares two modes. Its multi-viewer weaving
used to be a third mode identical to mode 1 except for the weaver option. It is now the
`MULTI_VIEWER` setting (`system|on|off`), read once per device creation, with a
`get_setting_status` answer (displayxr-leia-plugin `src/drv_leia/leia_multi_viewer.h`, #446, #575).
**It stays a driver setting and does not become a declared entry again.** It changes no geometry,
its `system` value follows a vendor system setting that a fixed entry cannot express, and it applies
to whichever 3D entry is in slot 1. Making it apply live is the driver's own settings hook (C2.3),
which is a vendor follow-up, not this ADR. Until then the Dashboard's "next time a 3D app starts"
note is accurate for it. For the vendor plug-in, then, nothing in C1 changes.

**C1.6 The signal: a new event, `XrEventDataRenderingModeTableChangedDXR`.** This ADR's first draft
re-used `XrEventDataRenderingModeChangedDXR` with an unchanged index. The cross-box review showed
that most consumers do the wrong thing with that event, and the reuse was rejected (§ *Alternatives
considered*). The decision:

- **A new event type** in an `XR_DXR_display_info` revision (the next `SPEC_VERSION`):
  `XrEventDataRenderingModeTableChangedDXR { type, next, session, activeModeIndex, flags }`.
  - It means "the rendering-mode table this session sees changed. Re-read
    `xrEnumerateDisplayRenderingModesDXR` and retile; the active index is unchanged."
  - `flags` has one bit, `XR_RENDERING_MODE_TABLE_CHANGE_RECREATE_SWAPCHAINS_BIT_DXR`, set only by
    C2. Without it, the swapchain is still large enough and the app must not reallocate (INV-4.9).
- **It is sent for:**
  - a 3D-mode switch (C1);
  - a window moving to a screen whose view size differs (ADR-047 Amendment 10);
  - the tracked flag changing (as well as the eye-tracking-state event);
  - C2, with the recreate bit.
- **Ordering and disambiguation.**
  - It is queued after the enumerate already answers the new table.
  - It is never the answer to `xrRequestDisplayRenderingModeDXR`. That answer stays the mode-changed
    event or a denial. "No mode-changed event means the request did not take" (`oxr_api_session.c`
    step 5, around line 1706 on main) keeps its meaning, because nothing unsolicited ever arrives as
    a mode-changed event.
  - When a table change and a mode change coincide, the runtime queues the mode-changed event first,
    then the table-changed event carrying the new index.
  - Under a workspace controller the mode-changed event may be reverted (spec v17, "treat the latest
    event as truth"). A table-changed event is sent only for a table already applied and is never
    reverted.
- **Rate.** At most one per session per second, coalesced to the latest table, like the Khronos
  extension's 1 Hz rule. A window parked across a seam while screens hotplug therefore cannot burst.
- **The mode-changed event keeps meaning exactly "the active index changed".** The runtime never
  sends it with an unchanged index.

**Who handles the new event today: nobody.** Every consumer needs a change, and the change is small
where it already re-reads on the mode-changed event. Verified on each repo's `main`, 2026-11-23:

| consumer | today, on `XrEventDataRenderingModeChangedDXR` | needed for the new event |
|---|---|---|
| displayxr-common (`xr_session_common.cpp` ~558) | re-reads the table since v2.34.0 (`ModeTable`), no swapchain recreation | a `case` calling the same refresh; recreate on the C2 bit; size from the worst-case statement (below) |
| demos, Windows legs (`windows/xr_session.h` on common v2.36.0) | follow common | re-pin common |
| modelviewer, earthview, gaussiansplat, avatar: macOS legs (`macos/main.mm`) | store the index only | a handler, or move onto common's session helper |
| modelviewer, earthview: Linux legs (`linux/main.cpp`) | store the index only | same |
| avatar: Linux leg (`linux/main.cpp:2058`) | re-reads (`RefreshRenderingModes`) | a `case` calling it |
| mediaplayer (`src/xr/XrSession.cpp:1130`, its own session code on every platform; common v2.36.0 used for header-only helpers) | stores the index only | a handler |
| Unity (`displayxr_provider_session.h`, `DisplayXRProvider.cs:395-398`) | native re-reads and re-derives tiling, then fires C# `RenderingModeChanged(prev, cur)` | native: the same refresh; do **not** fire `RenderingModeChanged` for it (a script would see `prev == cur`); C2 bit: its rebuild path |
| Unreal (`DisplayXRSession.cpp:1993-2003`) | re-enumerates (`RefreshRenderingModes`), no swapchain rebuild | a `case` calling it; C2 bit: a rebuild (none exists) |
| the browser (patches 0127, 0296) | forwards only `currentModeIndex` to pages | re-read in the browser; do **not** forward to pages as a mode change |
| `test_apps` in this repo | follow common's session helper | covered by the common change |

Older binaries that never learn the event keep their old tile size, and the segment DPs resample: the
picture is soft, never wrong. Stage 3 ships the event together with these consumer updates and the
demo re-pins (§ *Roadmap*).

**The worst-case statement.** The same revision adds a read-only, app-visible statement: the atlas
size and the view count over the full lists of every claimed screen, as a chained output struct on
`xrEnumerateDisplayRenderingModesDXR` or on `XrSystemProperties` (open question). It is an extension
struct for apps, not a driver ABI field. INV-4.2 then reads "size at the worst case the runtime
states".

It is required, not optional. displayxr-common sizes the swapchain from the **session screen's**
`displayPixelWidth/Height` × the enumerated modes (`common/xr_session_common.cpp:270-300`). A window
moved to a larger screen (ADR-047 Amendment 10), or a 3D mode larger than the current one, would
overflow that swapchain. The statement therefore ships in the same stage as "the view size follows
the screen" (stage 3), before any live 3D-mode switch (stage 4).

**C1.7 Apps that ignore the event.**
- **A stale view scale is resampled, not corrupted.** The compositor crops by the `imageRect`s the
  app actually submits (ADR-030), and the DP resamples that crop to the mode's view size.
- **The view count follows ADR-041.** The session's located count is the list's maximum and is fixed.
  `XrViewActivityStateDXR.activeViewCount` says how many views the active slot uses, and the app
  submits the located count with the inactive tail aliased onto view 0's subimage
  (`docs/reference/view-configuration-model.md` § *Submit the located count, alias the tail*). If an
  app misses a switch, the compositor-side clamp (same doc, § *The compositor-side clamp*) resolves
  the one-frame skew. An app that kept a 2-view atlas when the 3D mode went to Quad has its first two
  tiles shown and the rest cleared, which is the existing under-submit outcome.
- **A `PRIMARY_STEREO` session, a legacy app, an engine plug-in or the browser facing a 4-view
  slot 1** is floored to 2D, and a request for slot 1 is denied with `VIEW_CONFIG_CANNOT_FILL_DXR`.
  These are ADR-065 decision 6 and display_info v20, unchanged. If the switch to such a 3D mode
  happens while the session is in 3D, the same floor applies at the switch, and the app is told with
  the mode-changed event (index 1 → 0). This is the one case where a switch moves the app's mode,
  because the app cannot fill it.

**C1.8 What the list-wide view count does to existing apps.** It restores the pre-ADR-065 count.
With Quad out of sim_display's list by default (C1.2), sim_display's maximum stays 2, so the default
configuration changes nothing here. With Quad selected at start-up, or with a driver that declares a
wider entry:
- **Conformant `PRIMARY_MULTIVIEW_DXR` apps** (every app on `test_apps/common/dxr_view_config.h`'s
  `DxrAliasInactiveViews`, displayxr-common) locate the maximum, render `activeViewCount`, and alias
  the rest. Their per-view resources are sized for the maximum. That costs memory, not frame time.
- **An app that renders every located view** instead of reading `activeViewCount` does more work.
  ADR-041 allows this, but it is wasteful.
- **An app that submits the active count instead of the located count** is refused at the default
  `DXR_UNDER_SUBMIT=1`, except in the one-view case ("2 of 4" is refused; `view-configuration-model.md`
  § *DXR_UNDER_SUBMIT*). The demos through displayxr-common derive their view count from the
  rendering mode (stage 1b), so this is the case to audit before Quad, or any wider entry, can be in
  a default list.
- **`PRIMARY_STEREO` sessions** are unaffected: 2 located views whatever the list.

**C1.9 How a 3D-mode switch applies live.**
1. **The runtime remaps slot 1.** It re-reads the per-screen 3D-mode setting on the DIAG call that
   `dp mode set` makes, or on the service's component-policy watch for edits made elsewhere
   (`service_component_watch.c`, `plugin-discovery.md` §4.2a, widened to the 3D-mode keys). No driver
   slot is involved: the driver sees only an ordinary `OUTPUT_MODE` request in its own numbering if
   the session is in slot 1.
2. **IPC clients follow.** The shared memory already carries the declared list
   (`ism->hmd.rendering_modes`, up to 8 entries, copied at connect in `ipc_client_hmd.c:362-366`).
   What moves is the slot mapping, published with a generation counter that each IPC client checks
   and re-copies. This replaces the connect-time-only copy.
3. **Each session is told.** The enumerate answers from the new slots, then the table-changed event
   is queued. The active index does not change: the app owns its 2D/3D state (ADR-047 Amendment 10
   principle). An app in 3D stays in 3D and is now shown in the new 3D mode.
4. **Tracked or untracked.** If the active slot's tracked flag changed, the existing
   `XrEventDataEyeTrackingStateChangedDXR` is sent on the real edge, as for any tracking change
   (v14).

### C2. Recreation: only for a change outside the declared list

This path is for:
- a new driver hot-plugged or installed while apps run (ADR-047 Amendment 8);
- a driver update whose declared list holds a larger entry;
- a driver whose settings change its declared list itself (sim_display's Quad at start-up is the
  first example), when the driver supports applying that live.

C2.1 **The table-changed event with the recreate bit** (C1.6). It is queued after the new list,
slots and worst-case statement are visible to the enumerate, as with #1488's views-change event. One
event type serves both cases, because the app must re-read the table in both and recreating is one
more step in one branch. The bit, not a separate type, carries the difference. An app that only
retiles never sees the bit set outside C2.

C2.2 **IPC resync** of the declared list itself (not only the slot mapping), with the same
generation counter as C1.9 step 2. Service-created swapchains are recreated through the existing
create and destroy calls.

C2.3 **A driver settings re-read hook**, for the third case above and for a driver's own non-mode
settings applied live (the vendor plug-in's `MULTI_VIEWER`, C1.5): a new appended slot,
`settings_changed(inst, const char *key)` (ADR-020, `XRT_PLUGIN_IFACE_HAS_SETTINGS_CHANGED`). After
it, the runtime re-reads the driver's list. A driver without the slot keeps today's rule: such a
setting applies at the next start (ADR-054 Amendment 3's `RESTART_REQUIRED` status). 3D-mode
switches (C1) do not need this slot.

C2.4 **Reallocation of the runtime's own worst-case buffers** (the list in C1.5), at a frame
boundary, for the clients that recreated. A client that did not recreate keeps its old buffers
(C3).

C2.5 **displayxr-common recreates when the bit is set.** It destroys and recreates its swapchains at
the new worst case with the per-API drain it already does on teardown, and re-derives the tiling.
Every demo and test app on it gets this from one change.

C2.6 **The app rules and the linter change.** INV-4.2 ("size once") and INV-4.9 ("never reallocate
from the event handler") gain one exception: `xrCreateSwapchain` is allowed in the
table-changed handler's branch that tests the recreate bit, and only there.
`scripts/check_displayxr_app.py` accepts `xrCreateSwapchain` in a table-changed handler region that
names the bit, and keeps warning in the `XR_EXT_view_configuration_views_change` handler, the
mode-changed handler, and a table-changed handler that does not test the bit.

C2.7 **Engines and the browser, as follow-ups to file in their repos (listed here, not filed):**
- `displayxr-unity`: handle the new event natively (refresh only, no C# `RenderingModeChanged`), and
  map the recreate bit onto its existing rebuild path.
- `displayxr-unreal`: handle the new event (its existing `RefreshRenderingModes`), and add a
  swapchain rebuild for the bit.
- `displayxr-browser-pvt`: re-read on the new event without forwarding it to pages; rebuild on the
  bit. It builds exactly two views and rejects wider modes.
- `displayxr-common`: C1.6's handler and worst-case sizing, plus C2.5.
- the five demos: the handlers in C1.6's table and a common re-pin.
- `displayxr-shell-pvt`: check that the workspace controller's own swapchains follow (it drives the
  mode for its clients under the panel lease).

C2.8 **`maxImageRect*` is an open question.** Core OpenXR freezes the enumerate for the life of the
instance and the Khronos extension never moves `max*`, so C2 cannot raise `maxImageRect*`. Two
options:
- **(a)** DXR apps size from the worst-case statement, not from `maxImageRect*`, and the runtime
  accepts view rects above `maxImageRect*` from a session that enabled the revision.
- **(b)** `maxImageRect*` is reported generously at instance creation, for example at the device's
  maximum texture size. This costs memory for apps that size at max.

### C3. Apps that never answer

- **The runtime keeps the old swapchain and resamples it into the new mode.** The result is soft,
  not black. A recreate-bit event that gets no recreation leaves that client on its old buffers; its
  crop is resampled to the new view size as in C1.7. No timeout kills the session.
- **A view count above the instance's `PRIMARY_MULTIVIEW_DXR` count cannot happen inside an
  instance.** The located count is fixed. A new driver or a new declared entry that needs more views
  than the instance was created with:
  - is shown by the Dashboard as "restart the app to apply" (`applies: restart_app`), naming the
    running clients from the status snapshot (ADR-051);
  - can, for apps that opt in at instance creation (mechanism: open question), lead to instance loss
    (`XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING`) so that the app re-creates its instance at the new
    count. displayxr-common treats instance loss as exit today (`xr_session_common.cpp` ~554), so an
    opt-in app needs a relaunch loop;
  - for a stereo app facing an N > 2-view 3D mode, is floored to 2D by the existing denial and floor
    (C1.7).

## Alternatives considered

- **Re-use `XrEventDataRenderingModeChangedDXR` with an unchanged index** (this ADR's first-draft
  recommendation). Rejected after the cross-box review, for three reasons:
  - **Most consumers would do the wrong thing.** Of the consumers in C1.6's table, only
    displayxr-common's helper, the avatar Linux leg, Unity's native side and Unreal re-read the table
    on that event. Every other one stores the index and would stay stale anyway, so the reuse saves
    little code.
  - **It leaks a spurious event to user code.** Unity fires C# `RenderingModeChanged(prev, cur)` with
    `prev == cur`, and the browser forwards a same-mode event to web pages. Scripts and pages that run
    a 2D↔3D transition on it would misbehave.
  - **It blurs the request contract.** "No mode-changed event means the request did not take" is
    documented runtime behaviour (`oxr_api_session.c`, step 5). An unsolicited equal-index event
    arriving while a request is pending could be read as its answer.

  A new type costs one `case` in each consumer that already re-reads, and nothing for an old binary,
  which simply never sees it.
- **Two new event types** (one for retile, one for recreate). Rejected in favour of one type with a
  recreate bit (C2.1): both mean "re-read the table", and one handler with one branch is simpler.
- **A separate envelope declared by the driver** (a new `get_mode_envelope` slot returning the max
  view count and worst-case atlas over every config its settings can select, with configs built by
  the driver from its settings). Rejected in favour of C1. The full mode list the driver already
  declares carries exactly that information, so a second declaration could only disagree with it. It
  would also need a new ABI slot plus a driver re-read hook for every 3D-mode switch, where C1 needs
  neither.
- **Envelope from settings metadata** (a mode descriptor on each ENUM value of `describe_settings`).
  Rejected for the same reason, and because the frozen `xrt_component_setting_desc` cannot carry it
  (ADR-054 Amendment 3 kept it frozen).
- **Multi-viewer weaving as a declared entry again** (a live switch for free). Rejected (C1.5): it is
  a weaver policy with a `system` value, not a view layout.
- **Sparse or tiled virtual swapchains** (reserve the worst case's address space, commit pages on
  demand). Rejected. Support is uneven across platforms: D3D11 tiled resources are an optional tier,
  Metal sparse textures are limited, GL has only a vendor extension. Swapchains also cross processes
  as shared handles (IPC, `_texture`, the #918 egress), and sharing a partially committed resource is
  not portable. It would also save memory that C1 shows is mostly already spent on the 2D mode.
- **Runtime-only resampling with no app change.** Kept as the **fallback** (C1.7, C3), not the
  answer. Rendering at one size and resampling to another loses sharpness: "Same as 2D" exists to
  give a full-resolution picture, and a half-resolution render upsampled defeats it.
- **Per-screen swapchains** (one swapchain per screen or per segment). Rejected: it is a bigger app
  change than recreation (N swapchains, N submissions, per-screen view routing in every app), and the
  per-segment views of ADR-047 already put several screens' views in one atlas.
- **Restart only** (the ADR-065 status quo). Rejected by the project lead: a user switching the 3D
  mode in the Dashboard expects to see it, not to restart every 3D app.
- **Back to apps seeing N modes** (undo ADR-065). Rejected: ADR-065's reasons stand (no shipping
  hardware needs more than one 3D kind at a time, one key map, a user choice rather than an app's).
  C1 restores only the sizing, not the N-entry enumerate.

## Consequences

- On sim_display every 3D-mode switch applies live, except Quad, which is declared only at start-up
  for now. Its default 3D mode is "Same as 2D".
- The vendor plug-in is unaffected (C1.5).
- Driver-facing changes:
  - drivers may again declare their full list;
  - one `mode_flags` bit, `3D_PASSTHROUGH`;
  - one appended slot, `settings_changed` (C2.3);
  - the tracking plug-in's availability reason (Part B).

  There is no struct change and no ABI bump (ADR-020, ADR-022).
- `XR_DXR_display_info` takes revisions for:
  - `XrEventDataRenderingModeTableChangedDXR` and the worst-case statement (stage 3);
  - the recreate bit's semantics (stage 5).

  Each takes the next `SPEC_VERSION` when it lands. The spec carries a "proposed, not implemented"
  note until then (`docs/specs/extensions/XR_DXR_display_info.md` § *Proposed*).
- The app rules gain one exception (C2.6). INV-4.2 says "size at the stated worst case" (C1.6).
- The CLI gains `dp mode list|set|reset` and `tracking set|reset --screen`. `status --json` gains
  `mode_3d` and `viewer_tracking` per screen. Keys are only added.

## Roadmap

**Merge gate.** No code for parts A, B or C (ADR-047 Amendment 10 and this ADR) merges to `main`
before runtime **v2.70.5**, the QA build, is tagged. Design and docs PRs may merge earlier.

Each stage has a two-screen acceptance test on **ds1-linux** (the built-in 3D panel claimed by the
vendor plug-in, plus the laptop screen claimed by sim_display) and on the **Windows reference box**
(the 3D panel plus a second 3D monitor, with a per-screen preference giving the second screen to
sim_display; the shape #2218's crossing test used). **Android is out of scope**: it is single-screen.

**Stage 1 — per-screen rendering everywhere.**
- **Work:**
  - #2757 (an off-panel in-process Wayland window takes its own screen's segment: eyes + Kooima);
  - #2791, in-process native Wayland windows woven per screen (before it the manager runs on Wayland
    but records nothing per segment and holds segment DPs 2D);
  - the open #2218 items that gate this design: #7 and #1b's IPC half (`CLIENT_TEXTURE` and pinned
    IPC clients woven per screen), #8 / #8b (a verified claim beats the global active plug-in), #10
    (view-set capacity re-evaluated after a hotplug), #11 (declined vendors re-probed on Windows),
    #14 (per-screen metadata follows the per-screen DP; ADR-047 Amendment 10 generalises it to every
    screen), #15 (the app's MANAGED / MANUAL mode reaches segment DPs).
- **Already done:** #1 (#2224), #2 (#2223, #2228), #3 (#2232), #4 (#2249, #2253), #6 (#2248), #9
  (#2230), #16 (#2256).
- **Not gating:** #12 / #13 (status rows), #19 (diagnostics), #17 / #18 phases B and C (#2245).
- **Follow-ups from the #2791 review:**
  - the runtime's own per-frame locate (`oxr_session_frame_view_cameras`) always gets one view set,
    even when the app's locate is split;
  - the split is decided per locate call, so two locates in one frame can disagree;
  - a Windows run of Media Player straddling two DPs.
- **Acceptance:**
  - *ds1-linux:* in-process `cube_handle_vk_linux` on native Wayland and on X11, the same app under
    `XRT_FORCE_MODE=ipc`, and a weave present-owner. Move each from the panel to the laptop and
    back, then park it straddling. Each part is woven or shown flat by its own screen's DP (the
    vk_native and weave-engine atlas captures). The laptop part's eyes are nominal, the panel's are
    tracked. `displayxr-cli status --watch --json` files the window's DP rows under the right
    screens.
  - *Windows:* #2218's two-vendor crossing test, plus a zones app and a `CLIENT_TEXTURE` client
    dragged across.

**Stage 1b — every app kind on two screens (the validation gate for stage 1).**

The multi-screen work to date was validated with the `cube_*` test apps only. The first run of a
shipping demo across two screens (2026-11-23, #2791) found two defects the test apps hide:

- **Apps that submit one view set.** The demos, through displayxr-common, derive their view count
  from the rendering mode and chain neither `XrViewActivityStateDXR` nor
  `XrViewDisplayBindingsDXR`. Routed as per-segment sets, their one set was drawn in every
  segment. They now keep one view set, cropped per segment (#2791, state tracker, every platform).
- **A straddling Wayland buffer.** It follows the compositor's preferred scale (the highest among
  the outputs it touches), not its monitor's; the segments were cut in the wrong pixel space.

So stage 1 is not done until this matrix passes. Each cell is three placements — **wholly on the
3D panel, wholly on the other screen, straddling** (both majorities) — in 2D and in 3D.

| app kind | Linux Wayland | Linux X11 | Windows | macOS |
|---|---|---|---|---|
| `cube_*` test apps (read the activity state) | in-process, forced IPC | in-process, forced IPC | every compositor | Metal, Vulkan |
| demos on displayxr-common (one view set): modelviewer, mediaplayer, gaussiansplat, avatar, earthview | all | all | all | all |
| the DisplayXR Browser (present-owner weave, zones) | yes | yes | yes | yes (it weaves through the macOS service weave engine) |
| engine plug-ins (Unity, Unreal; fixed stereo) | Unity | Unity | both | Unity |
| legacy / `PRIMARY_STEREO` apps (`hello_xr`, the conformance suite's interactive tests) | yes | yes | yes | yes |
| workspace clients under a controller (the Shell) | yes | yes | yes | yes |
| `_texture` / `CLIENT_TEXTURE` clients | — | — | yes | Metal |

**Placements and topologies beyond the basic two screens** (each run against a representative
subset of the matrix: a `cube_*` app, one demo, the browser):
- a **rotated** screen (portrait next to landscape);
- **mixed and fractional DPI** (100 % next to 150 % or 166.67 %; the Wayland preferred-scale defect
  above is this class);
- **three or more screens**, including a window that covers three (more segments than
  `XRT_MAX_SEGMENTS`, ADR-047 Amendment 10 decision 5);
- **hotplug mid-straddle**: unplug and re-plug the screen holding the minority part while the
  window straddles;
- **under the Shell**: a table-changed event (stages 3 and 4) arriving while the workspace
  controller holds the lease and reverses a mode change (spec v17);
- **event bursts**: a window parked across a seam while a screen is hot-plugged and re-plugged, to
  check the 1 Hz coalescing (C1.6).

- **The check is capture-based, not an eyeball.** Per cell: the atlas and per-segment DP-input
  captures (`displayxr_atlas[.seg<k>].png`, or the weave engine's trio), a grab of each output
  (on GNOME Wayland through the compositor's screen-cast service; the D3D11 service's
  `workspace_screenshot` on Windows), and the log lines naming the screen and DP of each segment.
- **Pass means:**
  - each segment's DP input is its own part of the scene;
  - the scene is continuous across the seam **in position**. A one-set app (ADR-047
    Amendment 10 decision 2) is not continuous in sharpness: its part on the screen with the smaller
    view scale is resampled, so a step in sharpness at the seam is expected and passes;
  - a part on a screen its DP can show 1:1 is drawn at 1:1, and nothing is drawn outside its
    segment;
  - the eyes of each part are its own screen's.
- **A person looks only at what a capture cannot show:** depth quality on the panel, tracking feel,
  flicker under motion.
- **Decision this stage forces:** whether displayxr-common learns per-segment view sets (chain the
  activity state and the bindings: correct perspective and full sharpness on each screen) or stays
  on the one-set path (the majority screen's perspective, the other part resampled). Until it does,
  every demo is a one-set app. This is an open question.

**Stage 2 — Part B, the per-screen tracking setting.**
- **Acceptance:**
  - *ds1-linux, with an onnx-estimator build:* `tracking set --screen <laptop> 3d`. A cube wholly on
    the laptop in 3D follows the webcam face, and the camera closes when the app goes to 2D. With
    `always` it stays open in 2D. The panel's views follow only the panel's tracker. Covering the
    webcam changes nothing on the panel, and leaving the panel's tracker changes nothing on the
    laptop.
  - With the default (stub) build, the Dashboard shows the control greyed out with "no face
    estimator in this build". On a box with no camera, it says "no camera". On the panel's screen it
    says "this display tracks the viewer itself". `status --json` shows `viewer_tracking` as in B5.
  - *Windows:* the same on the second screen, with its webcam.

**Stage 3 — ADR-047 Amendment 10 (the view size follows the window's screen), the table-changed
event and the worst-case statement.**
- **Work:**
  - the per-screen mode lists and the effective table (ADR-047 Amendment 10 § *What has to exist*);
  - `XrEventDataRenderingModeTableChangedDXR` and the worst-case statement, in one
    `XR_DXR_display_info` revision;
  - the consumer updates in C1.6's table: displayxr-common (handler + worst-case sizing), the five
    demos' handlers and common re-pins, and the Unity, Unreal and browser follow-ups (C2.7), at least
    for the retile case.

  The statement and the common sizing change ship in this stage, not later, because a window moving
  to a larger screen would otherwise overflow a swapchain sized from the session screen (C1.6).
- **Acceptance:**
  - *ds1-linux:* sim_display "Same as 2D" on the laptop. A displayxr-common app in 3D moved from the
    panel to the laptop gets one table-changed event and a table refresh, and no mode-changed event.
    The atlas capture shows full-resolution tiles, and the laptop shows a sharp single view. Moved
    back, it shows the panel's tile size. Straddling gives the per-dimension maximum. Dragging back
    and forth across the seam faster than 0.5 s sends no event. An app that ignores the event
    (`hello_xr`) still shows a correct, resampled picture.
  - Each updated demo and engine passes the same move on at least one platform.
  - *Windows:* the same with sim_display on the second screen, including the portrait-and-landscape
    sizing case (ADR-047 Amendment 10 decision 1).

**Stage 4 — C1, the full list kept and live 3D-mode switches.**
- **Acceptance:**
  - *ds1-linux:* with an app running on the laptop in 3D, the laptop's 3D mode is cycled
    Same as 2D → Anaglyph → Squeezed SBS → Same as 2D, with `dp mode set` and with the Dashboard
    picker. Each switch lands within about 1 s with a table-changed event, no `xrCreateSwapchain` in
    the app's log, and no service or app restart.
  - Anaglyph is tracked, with the eye-tracking-state event, when stage 2's tracking is on.
  - `dp mode set quad` is refused with `restart_app` and its reason. With Quad selected at start-up,
    a `PRIMARY_STEREO` app facing it is floored to 2D with the denial event, and a
    `PRIMARY_MULTIVIEW_DXR` app locates 4 views and reports `activeViewCount` 1 / 2 / 4.
  - V toggles between the two slots in every 3D mode, including Same as 2D.
  - `status --json` shows `mode_3d` as in C1.3, with `source` following the CLI, the machine store
    and the driver default.
  - *Windows:* the same against an IPC client and an in-process client.

**Stage 5 — C2 / C3, recreation outside the declared list.**
- **Acceptance:** a test driver (or sim_display's Quad applied live through `settings_changed`)
  that adds a larger entry to its list at run time.
  - A displayxr-common app gets the table-changed event with the recreate bit and recreates; its log
    shows one `xrCreateSwapchain` at the new size.
  - An app built without the handler keeps running on a resampled picture.
  - An entry needing more views than the instance shows "restart the app to apply" in the Dashboard;
    an opt-in app gets instance loss and comes back at the new count.
  - Both boxes, with an IPC client and an in-process client.

## Open questions

1. **Where the worst-case statement lives**: chained on the rendering-mode enumerate, or on
   `XrSystemProperties`?
2. **`maxImageRect*` under C2**: option (a) or (b) of C2.8.
3. **How an app opts in to instance loss** (C3): a chained struct at `xrCreateInstance`, or an
   extension of its own?
4. **Part B and the static advertisement**: when a screen's setting turns tracking on for an app
   whose instance advertised none, should `XrEventDataEyeTrackingStateChangedDXR` be sent, and should
   MANUAL become requestable?
5. **Camera ↔ screen association** (B2): built-in camera to internal panel by default, an explicit
   calibration placement otherwise? What about an external webcam on a monitor with no claim?
6. **In-process apps and live switches.** The 3D-mode re-read is triggered by the service's DIAG
   call and policy watch. An in-process app with no service running needs its own watch or a cheap
   per-frame generation check.
7. **Quad, permanently.** The interim decision keeps Quad out of sim_display's list unless it is
   selected at start-up. The permanent answer depends on the C1.8 audit of apps that submit the
   active count.
8. **Does displayxr-common adopt per-segment view sets** (`XrViewActivityStateDXR` +
   `XrViewDisplayBindingsDXR`), so the demos get each screen's own perspective and sharpness when
   straddling, or do the demos stay one-set apps? (Stage 1b.)
9. **Unity's C# surface for the new event**: a separate C# event (`RenderingModeTableChanged`), or
   nothing beyond the native refresh?
