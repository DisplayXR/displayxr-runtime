---
status: Accepted
date: 2026-11-22
deciders: project lead
issues: [2766]
related: [ADR-010, ADR-014, ADR-022, ADR-027, ADR-028, ADR-030, ADR-041]
---
# ADR-065: Two run-time rendering modes per display driver; the 3D kind is a start-up setting

**Status:** Accepted (2026-11-22), stage A of
[#2766](https://github.com/DisplayXR/displayxr-runtime/issues/2766). **Stage B is superseded by
ADR-066 C1** (implemented): the runtime keeps each driver's full declared list as the sizing
envelope, apps still see two slots, and the 3D kind (now "the screen's 3D mode", a runtime-owned
per-screen setting with the driver's own setting as its default) applies live by remapping slot 1,
announced with `XrEventDataRenderingModeTableChangedDXR` (`XR_DXR_display_info` v27). Decisions 1,
2 and 4 below are amended by it (nothing is dropped; sizing is over the full list; the kind applies
live), and sim_display's default kind is now **Same as 2D**, not anaglyph. The text below is the
ADR as accepted. Related:
[ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md) (size once, never recreate — kept),
[ADR-028](ADR-028-display-mode-recipe-vs-hardware-state.md) (the mode is the content recipe),
[ADR-041](ADR-041-fixed-view-count-with-per-frame-activity.md) (fixed view count),
[ADR-022](ADR-022-per-mode-capability-flags-frozen-enum-structs.md) (per-mode flags),
[ADR-014](ADR-014-shell-owns-rendering-mode.md) (the workspace owns the mode).

## In one paragraph

A display driver exposes **exactly two rendering modes at run time: mode 0 = 2D, mode 1 = 3D.**
Which 3D *kind* mode 1 is — anaglyph, side-by-side, a 4-view quilt, a weaver variant — is a
per-driver **setting**, read when the driver creates its device and therefore applied at the
next start of the runtime (service) or of the app (in-process). Nothing is rebuilt at run time:
the worst-case swapchain is sized once over 2D and the selected kind. One key, V, toggles 2D and
3D on every platform; 0 and 1 select the modes directly.

## Context

Before this decision a driver could declare up to `XRT_MAX_RENDERING_MODES` (8) modes, and apps
and number keys cycled through them. In practice:

- No shipping hardware needs more than one 3D mode selectable at run time. The vendor plug-in
  declares 2D plus one 2-view 3D mode on every hardware class; on one platform it adds an
  optional third mode with the same atlas that differs only in a weaver option.
- `sim_display` was the only real multi-mode table (2D, anaglyph, top-bottom, squeezed
  side-by-side, a 2x2 quad, interlaced; `blend` had no index at all). Those are development
  and inspection tools.
- The four platform key maps disagreed (Win32/XCB/SDL mapped 1/2/3 to modes 0/1/2; macOS
  mapped 0-4 and made V also cycle), and every compositor carried a `last_3d_mode_index` to
  remember which 3D mode V should return to.
- The state tracker used `modeIndex` as an array index in the request path while the
  enumerate compared the vendor's `mode_index` for `isActive`.

A live change of the 3D kind runs into values OpenXR fixes for the life of an instance
(`PRIMARY_MULTIVIEW_DXR`'s view count, `maxImageRect`), has no event that tells an app to
re-read the table and rebuild its swapchains, and would need new code in every app leg. Two
modes with the kind fixed at start-up needs none of that.

## Decision

1. **The two-mode contract.** A driver's run-time table is `[0] = 2D, [1] = 3D`, with
   `mode_index` equal to the array position. The runtime enforces it once, where it takes the
   head device's table (`t_instance_create_system`, before any compositor, IPC copy or oxr
   system reads it; again, idempotently, when a plug-in's display info is applied; and in the
   no-compositor CLI instance). The rule is `u_rendering_modes_trim`
   (`src/xrt/auxiliary/util/u_rendering_modes.h`):
   - one or two modes: left alone (only `mode_index` is renumbered, if needed);
   - more than two: keep mode 0, which must be the 1-view 2D mode, and ONE 3D mode — the one
     the device marks active at creation if it is 3D, else the first mode that is 3D
     (`hardware_display_3d` or `view_count >= 2`) — move it to index 1, translate the active
     index, recompute `hmd->view_count` from the trimmed table, and log one WARN naming the
     driver and what was dropped;
   - a long table whose mode 0 is not flat, or that has no 3D mode, is **refused**: kept as
     declared, with a WARN that it does not meet the contract. The runtime does not guess
     which entry is the flat mode.
   When the kept 3D mode is not the driver's own index 1, `XRT_DEVICE_PROPERTY_OUTPUT_MODE`
   crosses to the driver in its own numbering (the device's property functions are wrapped).
   `XRT_MAX_RENDERING_MODES`, struct layouts, the plug-in ABI, the extension structs, the
   SPEC_VERSION and the IPC protocol are unchanged: they all carry a count, and 2 was always
   legal.
2. **The 3D kind is a driver setting applied at start-up.** Declared through the existing
   component-settings contract (`describe_settings` + `get_setting`: environment, per-user,
   machine). A driver with one kind declares nothing new. `sim_display`'s kind is its existing
   `OUTPUT` setting; anaglyph stays the default kind (the only position-preserving one, so it
   composes with display zones), and `blend` finally has a run-time home.
3. **`sim_display` starts in 2D.** A PC with no 3D display shows a normal picture; V (or 1, or
   an app request) turns the chosen kind on. `START_3D=1` starts it in the kind. `FORCE_MODE`
   remains a developer pin, limited to 0 or 1.
4. **Sizing covers 2D and the selected kind.** `u_tiling_compute_system_atlas` takes its
   maximum over the two served modes. On every shipping configuration the full-resolution 2D
   mode sets the size, so nothing shrinks; but that is a property of today's tables, not a
   rule — a quilt larger than the panel makes the 3D mode the worst case, and such a driver is
   now sized for max(2D, the selected kind) rather than for its largest kind.
5. **No swapchain is ever rebuilt.** ADR-010 stays true: size once, at the worst case over
   the two modes, never recreate.
6. **Kinds with more than two views are inspection kinds.** With only {2D, kind}, a stereo
   session (`PRIMARY_STEREO`, legacy apps, the engine plug-ins, the browser, the conformance
   suite) cannot fill a 4-view kind: the session-begin floor drops it to **2D** and a request
   for mode 1 is denied (`XrEventDataDisplayModeRequestDeniedDXR`). Those floors are kept as
   they are; they are correct. A `PRIMARY_MULTIVIEW_DXR` session fills all the views. The quad
   kind stays, because it is the only N-view path that runs without hardware.
7. **One key map.** V toggles mode 0 <-> mode 1 on every platform, decided from the mode the
   device is in now; 0 and 1 request modes 0 and 1; 2-8 request nothing. `last_3d_mode_index`
   is gone: the 3D mode is always index 1. `legacy_app_tile_scaling` gates the 0/1 keys
   exactly as it gated 1/2/3. The keys follow the floor: V or 1 does nothing (one throttled
   INFO) when the session cannot fill mode 1 - in-process from the limit the state tracker
   publishes at `xrBeginSession`, in the service from the view count R the client's
   projection layers carry.

## Consequences

- **Apps and plug-ins need nothing.** Every app sizes its mode table from the enumerated
  count; digit keys past the count were already inert; every start-up fallback already meant
  "mode 1 is the 3D mode". An older plug-in with a longer table is trimmed, with a WARN, and
  keeps working (see the forwarded-call note below); plug-ins should still declare two modes.
- **Observable changes.** `xrEnumerateDisplayRenderingModesDXR` returns two entries.
  `PRIMARY_MULTIVIEW_DXR`'s view count is the selected kind's (2 on sim_display's default
  kind, 4 only with the quad kind). sim_display sessions begin in 2D. A stereo session against
  the quad kind is floored to 2D, not to a 2-view 3D mode. A legacy app on a device that
  starts in 2D is still sized for the 3D mode V toggles into
  (`oxr_legacy_compromise_mode_index`), so its default sizing does not change.
- **Recipes that need 3D at start set the kind and the start.** The generated dev run
  scripts on every platform, the conformance `EyeVisibility` pass (`OUTPUT=sbs START_3D=1`).
  A legacy app's first frame on sim_display is 2D: one compromise-scale view (0.5 x 1.0 with
  the anaglyph kind) stretched across the window, until V.
- **An older plug-in with more modes keeps working.** While the runtime forwards an
  OUTPUT_MODE request in the driver's own numbering, the device shows the driver its own
  mode count, under a lock, so a driver that range-checks the index accepts it.
- **Changing the kind costs a restart.** For developers selecting an inspection kind that is a
  restart; owners of a shipping display never change it.

## Deferred: changing the kind while apps run (stage B)

> **Superseded by ADR-066 C1 (implemented).** The envelope is the list each driver already
> declares, so no swapchain is recreated for any entry of it; see the status note above.

Not decided here. It would need a new display-info event ("the mode table changed: re-read it
and recreate your swapchains") with a SPEC_VERSION bump and a defined outcome for apps that
never answer; a resync channel for the IPC copy of the table; a settings re-read hook on the
driver interface; reallocation of the runtime's own worst-case buffers; and swapchain
recreation in every app leg, which the app rules and the linter currently forbid. Kinds with
different view counts could still not change inside an instance (that is instance loss). It
would buy switching the kind without restarting 3D apps; it would not buy a smaller atlas.

## Alternatives considered

- **Keep N modes, size for the selected one at run time.** Rejected: the allocation is fixed
  at instance creation and the view count with it; a live kind change has no signal (the index
  stays 1) and no safe outcome for apps that never rebuild.
- **Clamp the table to two modes in each driver only.** Rejected as the only measure: an old
  plug-in with three modes would keep exposing three. The runtime enforces the contract at the
  one apply point; drivers are updated to declare two anyway.
- **Raise or lower `XRT_MAX_RENDERING_MODES`.** Rejected: an ABI break for no gain.

## Amendment 1 (2026-11-26): the driver never decides an app's start mode

**Decision.** Decision 3 ("`sim_display` starts in 2D") is withdrawn. Project lead, 2026-11-26:
"sim display should NOT decide for the app, the app decides which mode it starts in."

- Every display driver starts in its 3D slot (index 1), as the hardware drivers always have (the
  Leia plug-in starts in `LEIA_MODE_3D`). `sim_display` now does the same.
- The **app** picks its mode: `xrRequestDisplayRenderingModeDXR` (or the user, with V / 0 / 1). A
  driver setting never chooses it for the app.
- `START_3D` is deprecated: still declared (so stored values and old recipes do not error), no
  effect, one WARN when set. The Dashboard drops its "Start in" control. `FORCE_MODE` stays: it
  is a developer pin, not a start policy.

**Why decision 3's reason is gone.** It existed so a PC with no 3D display would not open apps in
red/cyan. Since ADR-066 the default 3D mode is **Same as 2D** (one full-resolution passthrough
view), so slot 1 already looks like 2D on an ordinary screen. A user who picks a stereo kind
(anaglyph, SBS, ...) in the Dashboard expects to see it in the app at once, not after a key press.

**Unchanged.** Decisions 4-7 (sizing over the two modes, no swapchain rebuilds, the floors for
kinds with more views than a session can fill, the key map). A legacy app keeps its sizing rule
(`oxr_legacy_compromise_mode_index`); a session that cannot fill slot 1 is floored to 2D at begin
exactly as before.

