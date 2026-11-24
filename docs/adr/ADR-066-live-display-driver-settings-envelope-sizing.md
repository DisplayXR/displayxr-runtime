---
status: Proposed
date: 2026-11-23
deciders: project lead
issues: [2766, 2218]
related: [ADR-010, ADR-020, ADR-022, ADR-028, ADR-030, ADR-041, ADR-047, ADR-051, ADR-054, ADR-065]
---
# ADR-066: Display-driver settings apply live; envelope sizing first, swapchain recreation as the rare path

**Status:** Proposed (2026-11-23). The project lead approved the direction; it is open for
cross-box review. Nothing here is implemented.

**What it changes in other ADRs:**

- It **supersedes the deferred "stage B"** of [ADR-065](ADR-065-two-runtime-rendering-modes.md)
  (§ *Deferred: changing the kind while apps run*). Stage B becomes cheap because the envelope is
  the mode list each driver already declares (Part C1). Recreation is kept only for a change outside
  that list (Part C2).
- It **amends ADR-065 decision 1**: the trim keeps the full declared list internally instead of
  discarding it, and slot 1 is 3D by position (C1.2).
- It **amends ADR-065 decisions 2 and 4**: the kind is selected by a config that applies live, and
  sizing is taken over the full declared list (C1.3, C1.4).
- It **amends ADR-065 decision 5** ("no swapchain is ever rebuilt"; C2).
- It **amends [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md)'s "allocate once, never
  recreate"**. Allocating once stays the rule, and covers every config switch (C1). Recreation is
  allowed only on the new event of C2, which fires only when a change falls outside the declared
  list.

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
exactly **two slots**: slot 0 = a 2D entry and slot 1 = the entry the screen's **config** maps to it.
Switching config in the Dashboard or the CLI only **remaps the slots** and fires the existing
mode-changed event with its index unchanged. There is no re-sizing, no recreation and no restart.
Recreating swapchains is a separate, rare path with its own event, used only when a change falls
outside the declared list (a new or updated driver). An app that never answers either event keeps
working: what it submits is resampled, never corrupted.

## Context

ADR-065 fixed the run-time contract at two modes and made the 3D kind a start-up setting. A live
kind change was deferred because of four problems (ADR-065 § *Deferred*; #2766's review):

1. **Values fixed for an instance.** `PRIMARY_MULTIVIEW_DXR`'s view count and `maxImageRect*` are
   fixed at `xrCreateInstance`. Core OpenXR requires `xrEnumerateViewConfigurationViews` to return
   identical contents for the life of the instance. The Khronos carve-out,
   `XR_EXT_view_configuration_views_change`, may move only the `recommended*` fields
   (`docs/specs/extensions/XR_EXT_view_configuration_views_change.md` §3).
2. **No signal.** A kind change keeps index 1, so the mode-changed event (fired on an index change)
   says nothing.
3. **Copies taken once.** IPC clients copy the mode table once at connect
   (`ipc/client/ipc_client_hmd.c:362-366`). Drivers read their settings once per process.
4. **Apps never rebuild.** Every demo and test app reads the table once and never rebuilds its main
   swapchain. Unreal handles no mode events. Unity rebuilds on resize and on a 2D↔3D switch.

#2766 also found that **a smaller atlas buys nothing**. The 2D mode (one view at full panel
resolution) sets the allocation on every shipping table. A 2-view half-resolution mode, a squeezed
side-by-side and a 2 × 2 quilt each fit inside it (`docs/specs/runtime/multiview-tiling.md`
§ *Worked examples*).

These two findings together are the point of this ADR. Before ADR-065 the runtime already sized the
swapchain and the view count over every declared mode. ADR-065 trimmed the list to two entries at
start-up, and that trim is what made a live change look expensive. If the runtime keeps the full
list for sizing and shows apps only two slots of it, problems 1, 3 and 4 do not arise for any mode
the driver declared. The allocation does not grow on any shipping table. The view count is the
list's maximum, and ADR-041 already lets a session use fewer views per frame. The "re-read the
table" signal (problem 2) already has a consumer, displayxr-common.

## Part B — viewer tracking is a per-screen setting

**The decision.** The Dashboard and the CLI offer one **per-screen "viewer tracking" toggle**.
- It is implemented by the existing viewer-tracking provider route (ADR-054 N6,
  `docs/specs/runtime/tracking-provider-discovery.md`), with `sim-tracking` `SOURCE=webcam` (§8) as
  the in-tree reference.
- It is **not** a `sim_display` output option. sim_display's `WEBCAM_TRACKING` is a one-release
  deprecation alias (§8 *One core, two hosts*) and stays one.

B1. **The toggle gates arbitration step 2 per screen.** The arbitration stays §4's, per screen (§4b):
1. the DP's own eyes, if the DP tracks;
2. the best enabled provider, if the DP's viewer policy is `EXTERNAL_OK`;
3. the nominal viewer.

The toggle decides whether step 2 is allowed on that screen. Today one provider serves *every*
`EXTERNAL_OK` screen that has no tracking DP (§4b). With the toggle, it serves only the screens where
the toggle is on. A tracking DP still wins its own screen, so the toggle is not offered there (reason
"this display tracks the viewer itself"). A `FIXED` display is not offered either.

B2. **The toggle is offered only when the device has a compatible camera.** "Compatible" means a
front camera associated with that screen, which the provider can open and analyse. The provider
probes, without opening the camera (as `sim_tracking`'s probe already does, §8), and reports per
screen either *available* or *unavailable* with a reason:
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

B4. **Live for the views.** Toggling re-runs the per-screen arbitration, as a topology publish does
today (`target_instance.c::publish_screens_locked`, §4b *When it is evaluated*). The views of a
running app follow within a frame or so.
- The composed `XR_DXR_display_info` eye-tracking advertisement (§5) stays static per instance. An
  app told "no tracking" at instance creation can therefore receive tracked views. For a
  RENDER_READY app this is harmless. Whether to also queue `XrEventDataEyeTrackingStateChangedDXR` is
  open question 6.
- When the toggle changes the tracked flag of the active slot (Part C), the existing
  eye-tracking-state event fires.

B5. **What the front ends need.**
- **Status model (ADR-051).** Each screen row of `status --json` gains a `viewer_tracking` object
  next to the existing `eye_tracking.viewer_source / viewer_reason / viewer_policy /
  viewer_provider`: `{ "setting": "on" | "off" | "default", "available": bool, "reason": "…" }`.
  Keys are only added.
- **Dashboard** (`src/dashboard`, Displays page). The screen card shows the toggle, disabled with the
  reason when it is unavailable, beside the existing "Calibrate camera…" action
  (`Pages/DisplaysPage.cs:303`). The Dashboard reads `status --watch --json` as today and writes
  through the CLI, so it stays a dumb client (ADR-051).
- **CLI.** `displayxr-cli tracking on|off --screen <key>`, `tracking reset --screen <key>|all`,
  with the same screen keys as `dp use --screen`. Settings are per user, `--machine` writes HKLM /
  the machine store, and a machine lock wins, as for every component setting. `tracking list --json`
  gains per-screen availability. The service learns of the write through the DIAG re-probe call that
  `dp use --screen` already makes, and through the component-policy watch for edits made elsewhere
  (`plugin-discovery.md` §4.2a).
- **Migration.** Today `sim-tracking ENABLE=1` means "serve every `EXTERNAL_OK` screen". Its reading
  becomes "the default for every screen with no per-screen value", so existing setups keep working.

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
- `hardware_display_3d`, the lens state ([ADR-028](ADR-028-display-mode-recipe-vs-hardware-state.md)).

**C1.2 Configs and slots, and what changes in ADR-065's trim.** A **config** is a pair of entries
from the declared list: slot 0 (2D) and slot 1 (3D). Apps see only the two slots: the enumerate
returns two entries, with `modeIndex` 0 and 1, exactly as under ADR-065.

- **The trim becomes a selection.** Today `u_rendering_modes_trim`
  (`auxiliary/util/u_rendering_modes.h`) keeps entry 0 and one 3D entry and discards the rest. It
  changes to keep the whole declared list in the device (internally, for sizing and remapping) and
  to present the two entries the active config names. The `to_driver` / `from_driver` maps that
  ADR-065 already builds for a forwarded `XRT_DEVICE_PROPERTY_OUTPUT_MODE` request become the
  config's mapping: a request for slot 1 reaches the driver as "the entry slot 1 is mapped to", in
  the driver's own numbering.
- **Slot 1 is 3D by position.** `u_rendering_mode_is_3d()` (`u_rendering_modes.h:73-76`) defines "3D"
  by content: `hardware_display_3d || view_count >= 2`. A config whose 3D slot equals its 2D needs a
  one-view entry in slot 1, which is neither. The change:
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
- **Who picks the config.** The config is a **runtime-owned per-screen setting**: "slot 1 = entry k"
  (and, optionally, "slot 0 = entry j"), stored like `dp use --screen` and keyed by the same screen
  key. Its values are the declared entries' names. The runtime owns the mapping, so a config switch
  needs no new driver slot.
  - A driver may suggest a default config with a setting of its own. sim_display's `OUTPUT` keeps
    that role: it names the entry slot 1 maps to when the user has chosen nothing.
  - With no setting at all, slot 1 is the entry ADR-065's trim keeps today (the one the device marks
    active at creation if it is 3D, else the first 3D entry). A driver that declares only two modes
    therefore behaves exactly as under ADR-065.

sim_display's configs, from its declared list (`drivers/sim_display/sim_display_modes.h`, with the
passthrough entry added):

| config | slot 0 (2D) | slot 1 (3D) | slot 1 atlas vs the 2D size |
|---|---|---|---|
| `default` | 1 full-screen view, untracked | 1 full-screen view, untracked, `3D_PASSTHROUGH`: the laptop stays sharp while the app is in 3D | equal |
| `anaglyph` | 1 full-screen view, untracked | 2 views at half resolution (0.5 × 0.5, 2 × 1), tracked | half |
| `quad` | 1 full-screen view, untracked | 2 × 2 quilt of 4 views (0.5 × 0.5) | equal |

The other existing kinds (`sbs`, `squeezed_sbs`, `interlaced`, `blend`) are further entries of the
same list, and all fit inside the 2D size.

**C1.3 What is the same as the pre-ADR-065 N-mode array, and what is new.**

| | Pre-ADR-065 N-mode array | ADR-065 | This ADR |
|---|---|---|---|
| What the driver declares | every mode, up to 8 | should declare two; a longer list is trimmed | every mode, up to 8 (as before ADR-065) |
| What the runtime keeps | every mode | two entries; the rest discarded | every mode, internally |
| Swapchain size and `maxImageRect*` | max over every mode | max over the two kept entries | **max over every mode** (as before ADR-065) |
| `PRIMARY_MULTIVIEW_DXR` view count | max over every mode | the kept 3D entry's | **max over every mode** (as before ADR-065) |
| Who selects the 3D kind | apps (`xrRequestDisplayRenderingModeDXR` with any index) and digit keys 1–8 | a driver setting, read at start-up | **a user setting** (Dashboard / CLI), per screen, applied live |
| What apps see | N entries | two | **two slots** (as ADR-065) |
| Keys | per-platform maps, 1/2/3 | V toggles, 0 / 1 | V toggles, 0 / 1 (as ADR-065) |
| Changing the kind under a running app | an index change, with the mode-changed event | restart | **a slot remap, with the mode-changed event at an unchanged index** (C1.5) |
| Per screen | no | no | **yes**: each screen has its own config (ADR-047 Amendment 10) |

So sizing and the view count go back to what they were before ADR-065. What is new is who selects
the kind, that apps see two slots, that configs are per screen, and the change event.

**C1.4 What is sized over the full list.** The runtime takes the union over the declared lists of
all claimed screens' drivers (ADR-047 Amendment 10 decision 1):
- **`PRIMARY_MULTIVIEW_DXR`'s view count** = the maximum `view_count` over the lists
  (`u_rendering_modes_max_view_count`) × the view-set capacity (ADR-047 M3). For sim_display that is
  4, because of `quad`, whatever the current config.
- **`maxImageRect*`** and the live `recommendedImageRect*` ceiling.
- **The atlas apps allocate.** It needs an app-visible statement (C1.6).
- **The runtime's own worst-case buffers**: the renderer atlases (#2564,
  `comp_d3d11_renderer.h:380`, `comp_d3d12_renderer.h:521`), the #918 egress ring (`sys->xbridge`),
  the service-created swapchains of IPC clients, and the `_texture` shared surface (ADR-010).

`u_tiling_compute_system_atlas` already takes its maximum over whatever array it is given; it is
handed the full list again instead of the two presented slots. The cost is memory, and ADR-065
already notes that the 2D mode usually dominates it. On sim_display every entry fits inside the
full-screen 2D size, so **sim_display never needs a recreation**. The vendor plug-in declares two
modes, so its list is its table and nothing changes for it.

**C1.5 A config switch applies live.** A config change made in the Dashboard or with the CLI
follows these steps:

1. **The runtime remaps the slots.** It re-reads the per-screen config setting, on the DIAG call the
   CLI makes after a write or on the service's component-policy watch for edits made elsewhere
   (`service_component_watch.c`, `plugin-discovery.md` §4.2a, widened to the config keys). It points
   slot 1 (and slot 0, if the config pairs a different 2D entry) at the new entries. No driver slot is
   involved: the driver sees only an ordinary `OUTPUT_MODE` request in its own numbering if the
   session is in the remapped slot.
2. **IPC clients follow.** The shared memory already carries the declared list
   (`ism->hmd.rendering_modes`, up to 8 entries, copied at connect in `ipc_client_hmd.c:362-366`).
   What moves is the slot mapping, published with a generation counter that each IPC client checks
   and re-copies. This replaces the connect-time-only copy.
3. **Each session is told.** The enumerate answers from the new slots, then the signal below is
   queued. The active index does not change: the app owns its 2D/3D state (ADR-047 Amendment 10
   principle). An app in 3D stays in 3D and is now shown in the new config's 3D entry.
4. **Tracked or untracked.** If the active slot's tracked flag changed, the existing
   `XrEventDataEyeTrackingStateChangedDXR` fires on the real edge, as for any tracking change (v14).

ADR-054 Amendment 3's `get_setting_status` lets the Dashboard say "applies now" before the user
commits. For the runtime-owned config this is always true for an entry in the declared list.

**C1.6 The signal: reuse `XrEventDataRenderingModeChangedDXR` with the index unchanged.** This is
the recommendation. It is the same signal ADR-047 Amendment 10 uses when a window changes screens.
An `XR_DXR_display_info` revision (the next `SPEC_VERSION`) documents an equal-index event as "the
mode table changed: re-read it; your swapchain is still large enough".

Why reuse rather than add a new event:
- **displayxr-common already does the right thing.** It re-reads the table on every
  `XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR` (`common/xr_session_common.cpp` ~558), and
  `ModeTable::refresh` (`common/mode_table.h:62-66`) reports a view-count, tile or view-scale change
  at the same index as a change. Every demo and test app built on it follows a live config switch
  with no code change.
- **No new type** for engines and the browser to learn. Code that `case`s on the event and re-reads
  the table is already correct.
- **The action is the same in both cases.** Config switches and screen moves both mean "re-read and
  retile", never "reallocate". A new event is kept for the one case that asks for a different action
  (C2), so an app cannot confuse "retile" with "recreate".

The risk is an app that treats every mode-changed event as a 2D↔3D transition: it would play a
transition animation, or rebuild as Unity does. Rebuilding is harmless, only wasteful. Reading the
index, which did not change, is the documented fix.

The same revision adds a read-only, app-visible **worst-case statement**: the atlas size and view
count over the full list, as a chained output struct on `xrEnumerateDisplayRenderingModesDXR` or on
`XrSystemProperties` (open question 3). This is an extension struct for apps, not a driver ABI
field. INV-4.2 then reads "size at the worst case the runtime states". Today INV-4.2 sizes over the
enumerated modes, and so does displayxr-common (`common/xr_session_common.cpp:285-300`), which with
two slots would size over the current config only. On sim_display that makes no difference (every
entry fits the 2D size), but for a driver whose larger entry is not in the current config it does.

**C1.7 Apps that ignore the event.**
- **A stale view scale is resampled, not corrupted.** The compositor crops by the `imageRect`s the
  app actually submits (ADR-030), and the DP resamples that crop to the mode's view size.
- **The view count follows ADR-041.** The session's located count is the list's maximum and is fixed.
  `XrViewActivityStateDXR.activeViewCount` says how many views the active slot uses, and the app
  submits the located count with the inactive tail aliased onto view 0's subimage
  (`docs/reference/view-configuration-model.md` § *Submit the located count, alias the tail*). If an
  app misses a switch, the compositor-side clamp (same doc, § *The compositor-side clamp*) resolves
  the one-frame skew. An app that kept a 2-view atlas when the config went to `quad` has its first
  two tiles shown and the rest cleared, which is the existing under-submit outcome.
- **A `PRIMARY_STEREO` session, a legacy app, an engine plug-in or the browser facing a 4-view
  slot 1** is floored to 2D, and a request for slot 1 is denied with `VIEW_CONFIG_CANNOT_FILL_DXR`.
  These are ADR-065 decision 6 and display_info v20, unchanged. If the switch to such a config
  happens while the session is in 3D, the same floor applies at the switch, and the app is told with
  the mode-changed event (index 1 → 0). This is the one case where a switch moves the app's mode,
  because the app cannot fill it.

**C1.8 What the list-wide view count does to existing apps.** This is the main behaviour change of
C1 compared with ADR-065 (it restores the pre-ADR-065 count), and every consumer must be checked
before it ships:
- **Conformant `PRIMARY_MULTIVIEW_DXR` apps** (every app on `test_apps/common/dxr_view_config.h`'s
  `DxrAliasInactiveViews`, displayxr-common) locate 4 views on sim_display, render
  `activeViewCount` (1 or 2 in most configs), and alias the rest. Their per-view resources (depth
  buffers, per-view uniforms) are sized for 4. That costs memory, not frame time.
- **An app that renders every located view** instead of reading `activeViewCount` does up to 4× the
  work in the `default` and `anaglyph` configs. ADR-041 allows this, but it is wasteful.
- **An app that submits the active count instead of the located count** is refused at the default
  `DXR_UNDER_SUBMIT=1`, except in the one-view case ("2 of 4" is refused; `view-configuration-model.md`
  § *DXR_UNDER_SUBMIT*). Under ADR-065 such an app works on sim_display only because the count equals
  the selected kind's. This is the case to audit: the engine plug-ins (Unity is `PRIMARY_STEREO`; the
  Unreal plug-in's adaptive N-view path must be checked), the browser (two views, stereo) and the
  demos.
- **`PRIMARY_STEREO` sessions** are unaffected: 2 located views whatever the list.
- **The vendor plug-in**: it declares two modes, so nothing changes.

Whether sim_display should keep `quad` in its declared list by default, at the price of 4 located
views for every multiview session on a box without a 3D display, is open question 2.

### C2. Recreation: only for a change outside the declared list

This path is for:
- a new driver hot-plugged or installed while apps run (ADR-047 Amendment 8);
- a driver update whose declared list holds a larger entry;
- a driver whose settings change its declared list itself, not only the config.

C2.1 **A new event**, "the mode table changed: re-read it and recreate your swapchains"
(`XrEventDataRenderingModeTableChangedDXR`, working name), in an `XR_DXR_display_info` revision. It
is queued after the new list, slots and worst-case statement are visible to the enumerate, as with
#1488's views-change event. It is a new type rather than another equal-index mode-changed event so
that an app that only retiles cannot mistake it.

C2.2 **IPC resync** of the declared list itself (not only the slot mapping), with the same
generation counter as C1.5 step 2. Service-created swapchains are recreated through the existing
create and destroy calls.

C2.3 **A driver settings re-read hook**, for the third case above only: a new appended slot,
`settings_changed(inst, const char *key)` (ADR-020, `XRT_PLUGIN_IFACE_HAS_SETTINGS_CHANGED`), after
which the runtime re-reads the driver's list. A driver without the slot keeps today's rule: such a
setting applies at the next start (ADR-054 Amendment 3's `RESTART_REQUIRED` status). Config switches
(C1) do not need this slot.

C2.4 **Reallocation of the runtime's own worst-case buffers** (the list in C1.4), at a frame
boundary, for the clients that recreated. A client that did not recreate keeps its old buffers
(C3).

C2.5 **displayxr-common recreates on the new event.** It destroys and recreates its swapchains at
the new worst case with the per-API drain it already does on teardown, and re-derives the tiling.
Every demo and test app built on it gets this from one change. The in-repo `test_apps/common`
session helpers get the same handler.

C2.6 **The app rules and the linter change.** INV-4.2 ("size once") and INV-4.9 ("never reallocate
from the event handler") gain one exception: `xrCreateSwapchain` is allowed in the handler of the
C2 event, and only there. `scripts/check_displayxr_app.py` accepts `xrCreateSwapchain` inside that
handler region and keeps warning in the `XR_EXT_view_configuration_views_change` handler and the
mode-changed handler.

C2.7 **Engines and the browser, as follow-ups to file in their repos (listed here, not filed):**
- `displayxr-unity`: map the C2 event onto its existing rebuild path (it already rebuilds on resize
  and on 2D↔3D), and treat an equal-index mode-changed event as "retile only".
- `displayxr-unreal`: handle the mode-changed event at all (re-read and retile), then the C2 event
  (rebuild). It handles no mode events today.
- `displayxr-browser-pvt`: re-read on an equal-index mode-changed event, and rebuild on the C2 event.
  It builds exactly two views and rejects wider modes.
- `displayxr-common`: C2.5, plus the INV-4.2 worst-case sizing of C1.6.
- `displayxr-shell-pvt`: check that the workspace controller's own swapchains follow (it drives the
  mode for its clients under the panel lease).

C2.8 **`maxImageRect*` is open question 4.** Core OpenXR freezes the enumerate for the life of the
instance and the Khronos extension never moves `max*`, so C2 cannot raise `maxImageRect*`. Two
options:
- **(a)** DXR apps size from the worst-case statement, not from `maxImageRect*`, and the runtime
  accepts view rects above `maxImageRect*` from a session that enabled the revision.
- **(b)** `maxImageRect*` is reported generously at instance creation, for example at the device's
  maximum texture size. This costs memory for apps that size at max.

### C3. Apps that never answer

- **The runtime keeps the old swapchain and resamples it into the new mode.** The result is soft,
  not black. A C2 event that gets no recreation leaves that client on its old buffers; its crop is
  resampled to the new view size as in C1.7. No timeout kills the session.
- **A view count above the instance's `PRIMARY_MULTIVIEW_DXR` count cannot happen inside an
  instance.** The located count is fixed. A new driver or a new declared entry that needs more views
  than the instance was created with:
  - is shown by the Dashboard as "restart the app to apply", naming the running clients from the
    status snapshot (ADR-051);
  - can, for apps that opt in at instance creation (mechanism: open question 5), lead to instance
    loss (`XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING`) so that the app re-creates its instance at the
    new count. displayxr-common treats instance loss as exit today (`xr_session_common.cpp` ~554),
    so an opt-in app needs a relaunch loop;
  - for a stereo app facing an N > 2-view config, is floored to 2D by the existing denial and floor
    (C1.7).

## Alternatives considered

- **A separate envelope declared by the driver** (a new `get_mode_envelope` slot returning the max
  view count and worst-case atlas over every config its settings can select, with configs built by
  the driver from its settings). Rejected in favour of C1. The full mode list the driver already
  declares carries exactly that information, so a second declaration could only disagree with it. It
  would also need a new ABI slot plus a driver re-read hook for every config switch, where C1 needs
  neither.
- **Envelope from settings metadata** (a mode descriptor on each ENUM value of `describe_settings`).
  Rejected for the same reason, and because the frozen `xrt_component_setting_desc` cannot carry it
  (ADR-054 Amendment 3 kept it frozen).
- **Sparse or tiled virtual swapchains** (reserve the worst case's address space, commit pages on
  demand). Rejected. Support is uneven across platforms: D3D11 tiled resources are an optional tier,
  Metal sparse textures are limited, GL has only a vendor extension. Swapchains also cross processes
  as shared handles (IPC, `_texture`, the #918 egress), and sharing a partially committed resource is
  not portable. It would also save memory that C1 shows is mostly already spent on the 2D mode.
- **Runtime-only resampling with no app change.** Kept as the **fallback** (C1.7, C3), not the
  answer. Rendering at one size and resampling to another loses sharpness: the `default` config's
  point is a full-resolution picture, and a half-resolution render upsampled defeats it.
- **Per-screen swapchains** (one swapchain per screen or per segment). Rejected: it is a bigger app
  change than recreation (N swapchains, N submissions, per-screen view routing in every app), and the
  per-segment views of ADR-047 already put several screens' views in one atlas.
- **Restart only** (the ADR-065 status quo). Rejected by the project lead: a user switching a config
  in the Dashboard expects to see it, not to restart every 3D app.
- **Back to apps seeing N modes** (undo ADR-065). Rejected: ADR-065's reasons stand (no shipping
  hardware needs more than one 3D kind at a time, one key map, a user choice rather than an app's).
  C1 restores only the sizing, not the N-entry enumerate.
- **A new event for config switches too.** Rejected for C1 (C1.6): the action is the same as for an
  index change (re-read and retile), the existing event already has a correct consumer, and a second
  event type would be one more thing for every engine to learn for no different behaviour.

## Consequences

- On sim_display, every config switch applies live. Multiview sessions locate 4 views there (C1.8).
- The vendor plug-in is unaffected while it declares two modes. If it declares more entries (for
  example a weaver variant), switching between them becomes a live config switch for free.
- Driver-facing changes:
  - drivers may again declare their full list;
  - one `mode_flags` bit, `3D_PASSTHROUGH`;
  - one appended slot, `settings_changed`, needed only for C2's third case;
  - the tracking plug-in's availability reason (Part B).

  There is no struct change and no ABI bump (ADR-020, ADR-022).
- `XR_DXR_display_info` takes revisions for:
  - the equal-index event and the worst-case statement (C1, together with ADR-047 Amendment 10's
    signal);
  - the C2 event.

  Each takes the next `SPEC_VERSION` when it lands. The spec carries a "proposed, not implemented"
  note until then (`docs/specs/extensions/XR_DXR_display_info.md` § *Proposed*).
- The app rules gain one exception (C2.6). INV-4.2 says "size at the stated worst case" (C1.6).

## Roadmap

Each stage has a two-screen acceptance test on **ds1-linux** (the built-in 3D panel claimed by the
vendor plug-in, plus the laptop screen claimed by sim_display) and on the **Windows reference box**
(the 3D panel plus a second 3D monitor, with a per-screen preference giving the second screen to
sim_display; the shape #2218's crossing test used).

**Stage 1 — per-screen rendering everywhere.**
- **Work:**
  - #2757 (an off-panel in-process Wayland window takes its own screen's segment: eyes + Kooima);
  - its in-flight follow-up, in-process native Wayland windows woven per screen (today the
    manager runs on Wayland but records nothing per segment and holds segment DPs 2D);
  - the open #2218 items that gate this design: #7 and #1b's IPC half (`CLIENT_TEXTURE` and pinned
    IPC clients woven per screen), #8 / #8b (a verified claim beats the global active plug-in), #10
    (view-set capacity re-evaluated after a hotplug), #11 (declined vendors re-probed on Windows),
    #14 (per-screen metadata follows the per-screen DP; ADR-047 Amendment 10 generalises it to every
    screen), #15 (the app's MANAGED / MANUAL mode reaches segment DPs).
- **Already done:** #1 (#2224), #2 (#2223, #2228), #3 (#2232), #4 (#2249, #2253), #6 (#2248), #9
  (#2230), #16 (#2256).
- **Not gating:** #12 / #13 (status rows), #19 (diagnostics), #17 / #18 phases B and C (#2245).
- **Acceptance:**
  - *ds1-linux:* in-process `cube_handle_vk_linux` on native Wayland and on X11, the same app under
    `XRT_FORCE_MODE=ipc`, and a weave present-owner. Move each from the panel to the laptop and
    back, then park it straddling. Each part is woven or shown flat by its own screen's DP (the
    vk_native and weave-engine atlas captures). The laptop part's eyes are nominal, the panel's are
    tracked. `displayxr-cli status --watch --json` files the window's DP rows under the right
    screens.
  - *Windows:* #2218's two-vendor crossing test, plus a zones app and a `CLIENT_TEXTURE` client
    dragged across.

**Stage 2 — Part B, the per-screen tracking setting.**
- **Acceptance:**
  - *ds1-linux, with an onnx-estimator build:* `tracking on --screen <laptop>`. A cube wholly on the
    laptop follows the webcam face. The panel's views follow only the panel's tracker. Covering the
    webcam changes nothing on the panel, and leaving the panel's tracker changes nothing on the
    laptop.
  - With the default (stub) build, the Dashboard shows the toggle disabled with "no face estimator
    in this build". On a box with no camera, it says "no camera". On the panel's screen it says "this
    display tracks the viewer itself".
  - *Windows:* the same on the second screen, with its webcam.

**Stage 3 — ADR-047 Amendment 10, the view size follows the window's screen.**
- **Acceptance:**
  - *ds1-linux:* sim_display `default` config on the laptop. A displayxr-common app in 3D moved from
    the panel to the laptop gets one equal-index `Rendering mode changed 1 -> 1` and a table refresh.
    The atlas capture shows full-resolution tiles, and the laptop shows a sharp single view. Moved
    back, it shows the panel's tile size. Straddling gives the per-dimension maximum. Dragging back
    and forth across the seam faster than 0.5 s sends no event. An app that ignores the event
    (`hello_xr`) still shows a correct, resampled picture.
  - *Windows:* the same with sim_display on the second screen.

**Stage 4 — C1, the full list kept and live config switches.**
- **Acceptance:**
  - *ds1-linux:* with an app running on the laptop in 3D, the laptop's config is cycled `default` →
    `anaglyph` → `quad` → `default` from the CLI and from the Dashboard. Each switch lands within
    about 1 s with an equal-index event, no `xrCreateSwapchain` in the app's log, and no service or
    app restart.
  - `anaglyph` toggles tracked, with the eye-tracking-state event, when stage 2's tracking is on.
  - A `PRIMARY_STEREO` app facing `quad` is floored to 2D with the denial event.
  - A `PRIMARY_MULTIVIEW_DXR` app locates 4 views throughout and reports `activeViewCount` 1 / 2 / 4.
  - V toggles between the two slots in every config, including `default`.
  - *Windows:* the same against an IPC client and an in-process client.

**Stage 5 — C2 / C3, recreation outside the declared list.**
- **Acceptance:** a test driver (or a sim_display test setting) that adds a larger entry to its list
  at run time.
  - A displayxr-common app gets the C2 event and recreates; its log shows one `xrCreateSwapchain`
    at the new size.
  - An app built without the handler keeps running on a resampled picture.
  - An entry needing more views than the instance shows "restart the app to apply" in the Dashboard;
    an opt-in app gets instance loss and comes back at the new count.
  - Both boxes, with an IPC client and an in-process client.

## Open questions

1. **Config naming and the default.** Should sim_display's `OUTPUT` keep naming the default config,
   or should the runtime-owned config setting replace it with `OUTPUT` kept as an alias for one
   release? Should `default` (sharp, no 3D look) become sim_display's default, given ADR-065's
   start-in-2D reasoning?
2. **The 4-view count on sim_display.** Keep `quad` in sim_display's declared list by default (every
   multiview session locates 4 views on a box with no 3D display, C1.8), or declare it only when a
   start-up setting asks for it?
3. **Where the worst-case statement lives**: chained on the rendering-mode enumerate, or on
   `XrSystemProperties`?
4. **`maxImageRect*` under C2**: option (a) or (b) of C2.8.
5. **How an app opts in to instance loss** (C3): a chained struct at `xrCreateInstance`, or an
   extension of its own?
6. **Part B and the static advertisement**: when a screen's toggle turns tracking on for an app
   whose instance advertised none, should `XrEventDataEyeTrackingStateChangedDXR` fire, and should
   MANUAL become requestable?
7. **Camera ↔ screen association** (B2): built-in camera to internal panel by default, an explicit
   calibration placement otherwise? What about an external webcam on a monitor with no claim?
8. **In-process apps and live config switches.** The config re-read is triggered by the service's
   DIAG call and policy watch. An in-process app with no service running needs its own watch or a
   cheap per-frame generation check.
9. **A config pairing a different 2D entry**: should slot 0 be remappable at all, or always the
   declared list's entry 0?
