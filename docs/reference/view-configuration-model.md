# View-configuration model — `PRIMARY_STEREO` vs `PRIMARY_MULTIVIEW_DXR`

DisplayXR drives displays whose rendering modes span **1 to 4 views** (2D, stereo,
quad), while OpenXR makes the view count a property of the *view configuration*.
This page records how the runtime reconciles the two **as implemented**: which
types it advertises, how many views each reports, what `xrLocateViews` and
`xrEndFrame` do under each, and which knob restores the old behaviour.

> **The short version.** A stereo app does nothing and gets exactly 2 views from
> a conformant `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`. An app that wants the
> device's N-view modes enables `XR_DXR_display_info`, finds
> `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR` in
> `xrEnumerateViewConfigurations`, and begins its session with that type.
> Landed for [#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486)
> (option B) / [#80](https://github.com/DisplayXR/displayxr-runtime/issues/80).

## What the runtime advertises

The system carries a **list** of view configurations, not a single type
(`struct oxr_system`, `oxr_objects.h`: `view_config_count`,
`view_config_types[]`, `view_config_view_counts[]`), populated in
`oxr_system_fill_in()` (`src/xrt/state_trackers/oxr/oxr_system.c`):

| Device | `XR_DXR_display_info` enabled? | `xrEnumerateViewConfigurations` returns |
|---|---|---|
| `view_count == 1` (mono-only) | either | `PRIMARY_MONO` |
| `view_count >= 2` | **no** | `PRIMARY_STEREO` |
| `view_count >= 2` | **yes** | `PRIMARY_STEREO`, then `PRIMARY_MULTIVIEW_DXR` |

`PRIMARY_MONO` and `PRIMARY_STEREO` stay mutually exclusive, exactly as before.
`PRIMARY_MULTIVIEW_DXR` is **gated on the extension being enabled on the
instance** — an app that never asked for `XR_DXR_display_info` never sees a
vendor enum.

**Naming it anyway fails, but with two different codes depending on the entry
point**, because only some entry points run the validation whitelist
(`oxr_verify_view_config_type`, `oxr_verify.c:443`):

| Entry point | Runs the whitelist? | Result without the extension |
|---|---|---|
| `xrLocateViews` (`oxr_api_session.c:295`) | yes | `XR_ERROR_VALIDATION_FAILURE` |
| `xrGetVisibilityMaskKHR` (`:371`) | yes | `XR_ERROR_VALIDATION_FAILURE` |
| `xrEnumerateEnvironmentBlendModes` (`oxr_api_system.c:142`) | yes | `XR_ERROR_VALIDATION_FAILURE` |
| `xrBeginSession`, graphics-bound (`oxr_api_session.c:133`) | yes | `XR_ERROR_VALIDATION_FAILURE` |
| `xrBeginSession`, **headless** (`XR_MND_headless`, no compositor) | **no** | ignored — `primaryViewConfigurationType` is not checked at all |
| `xrEnumerateViewConfigurationViews` (`oxr_api_system.c:187`) | **no** | `XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED` |
| `xrGetViewConfigurationProperties` (`:170`) | **no** | `XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED` |

`XR_ERROR_VALIDATION_FAILURE` is the usual spec pattern for an extension enum
whose extension is not enabled. The bottom two go straight to
`oxr_system_lookup_view_config()`, so for them "the extension is off" and "the
system does not advertise it" are the same answer. A valid-but-not-advertised
type returns `XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED` everywhere. Apps
should branch on the `xrEnumerateViewConfigurations` result, not on an error
code.

`PRIMARY_MULTIVIEW_DXR` is advertised on **every** 3D-capable device when the
extension is on — including a stereo-only device such as Leia, where it reports
2. That is deliberate: an N-view-capable app can select it unconditionally
without branching on the hardware.

### The enum value

```c
// src/external/openxr_includes/openxr/XR_DXR_display_info.h (spec v19)
#define XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR ((XrViewConfigurationType)1004999212)
```

It lives in `XR_DXR_display_info`'s 210–219 decade of the registered `DXR`
block. It is a cast `#define` because C cannot extend the core enum — so it is
invisible to `-Wswitch`, and every `switch` over `XrViewConfigurationType` that
must handle it needs an explicit `case`.

## Per-type behaviour

| | `PRIMARY_MONO` | `PRIMARY_STEREO` | `PRIMARY_MULTIVIEW_DXR` |
|---|---|---|---|
| `xrEnumerateViewConfigurationViews` count | 1 | **2** | device **max across modes** (4 on sim-display, 2 on Leia) |
| `xrLocateViews` `*viewCountOutput` | 1 | **2** | same max |
| `xrLocateViews` capacity required | 1 | 2 | max (size to `XRT_MAX_VIEWS` = 8) |
| `xrEndFrame` projection `viewCount` accepted | 1 | **exactly 2** — the located count. An `XR_DXR_display_info` app may still submit 1 while a **1-view mode is in play** — the mode active now **or** the one latched at this frame's `xrBeginFrame` (#1528): **deprecated** (ADR-041), accepted, logged once per session | **exactly the located count** (the device max). ADR-041 removed the old "any rendering mode's `viewCount`" |
| Fixed for the instance lifetime? | yes | yes | yes |

> **A core-only app gets exact-2, full stop.** If the instance did not enable
> `XR_DXR_display_info`, `PRIMARY_STEREO` accepts `viewCount == 2` and nothing
> else, whatever rendering mode the panel happens to be in. That is the
> conformance contract: the CTS `XrCompositionLayerProjection` test decrements
> the located count (`test_XrCompositionLayerProjection.cpp:225-230`:
> `Layer.viewCount--`, then
> `CHECK(XR_ERROR_VALIDATION_FAILURE == endFrame(...))`).
>
> **The one-view allowance is an extension-scoped relaxation**, and it needs
> *both* halves: the instance enabled `XR_DXR_display_info` **and** the active
> rendering mode is itself 1-view. It exists for the 2D/mono submission path the
> extension has always allowed (`cube_*` apps compute
> `eyeCount = display3D ? modeViewCount : 1`; `displayxr-common` forwards the
> caller's count), and for no one else — a core-only app has no notion of a
> rendering mode at all: it cannot enumerate one, request one, or be told the
> active one changed, so scoping a relaxation to a fact it cannot observe would
> make `PRIMARY_STEREO`'s meaning depend on hidden runtime state.
>
> **Gating on the mode alone was not enough, and the win box proved it.** The
> first attempt allowed 1 in any 1-view mode regardless of extensions;
> `XrCompositionLayerProjection` still failed the full suite, because a CTS
> session never enables `XR_DXR_display_info` — so the runtime treats it as a
> legacy session and sim-display sits in a 1-view (Passthrough/2D) mode for
> essentially the whole run. The exception was open for the entire conformance
> pass. The extension gate closes it.
>
> **Mode-edge grace: "the active mode" means *when the frame was begun*, too**
> (#1528). A 2D→3D switch lands in the middle of a frame — the app begins the
> frame while the mode is 1-view, renders the one view it was told about, and by
> the time it calls `xrEndFrame` the runtime has already flipped the panel to
> 2-view. Judging only the live mode therefore rejected **exactly one frame per
> crossing** (measured on the win box: 4/4 crossings under Unity, and identically
> with the previous plugin build as a control). So the runtime latches the active
> mode's view count at `xrBeginFrame`
> (`oxr_session::frame_begin_mode_view_count`) and accepts `viewCount == 1` if
> *either* that latched count or the live one is 1.
>
> The grace is **one frame wide by construction** — the latch is overwritten at
> the next `xrBeginFrame`, so an app that keeps submitting 1 while the panel is
> in a 2-view mode is still refused from its second frame on. And it is
> deliberately runtime-side: the alternative is that every provider re-renders
> the in-flight frame with 2 views, which pushes a race the *runtime* owns onto
> every consumer. The extension gate is untouched — a core-only app gets exact-2
> at the mode edge as everywhere else.
>
> **`PRIMARY_MULTIVIEW_DXR` is the recommended path for any mode-driven count**,
> including the 1-view case: begin with it and submit the active mode's count
> without a special case. The relaxation above is back-compatibility for apps
> already shipping on `PRIMARY_STEREO`. (The permissive rule never consulted the
> active mode at all, precisely because of this race — #1528 gives the tight rule
> the narrow, one-frame version of the same concession, and ADR-041 keeps it.)
>
> This is the only place the `xrEndFrame` rule consults the active mode; the
> *reported* counts above still never move on a mode switch.

## Submit the located count, alias the tail (ADR-041)

The counts above are fixed for the session. What changes per frame is how many of
them the active rendering mode *uses*. ADR-041 separates the two properly:

- `xrLocateViews` publishes `activeViewCount` through **`XrViewActivityStateDXR`**
  (`XR_DXR_display_info` v21), chained on `XrViewState`.
- Views `[0, activeViewCount)` carry the active mode's poses/FOVs. Views
  `[activeViewCount, viewCountOutput)` are **inactive**: located at view 0's pose,
  and their submitted content is ignored.
- `xrEndFrame` accepts **exactly the located count**, for every type. An app that
  renders only the active views points each inactive view at content it already
  rendered this frame (view 0's subimage) while keeping that view's own located
  pose/FOV. `DxrAliasInactiveViews()` in `test_apps/common/dxr_view_config.h` is
  the reference tail fill.

That makes both core sentences hold verbatim — "`viewCount` must be equal to the
number of view poses returned by `xrLocateViews`" and "all views associated with
projection layers must be supplied" — with no DisplayXR carve-out. The pre-ADR-041
`PRIMARY_MULTIVIEW_DXR` rule ("any rendering mode's `viewCount`") contradicted
both, which is why it is gone.

**A 3D zone layer is a projection layer**: `XR_DXR_display_zones` submits each 3D
zone as an `XR_TYPE_COMPOSITION_LAYER_PROJECTION` with a zone chained on it, so it
goes through the same gate and carries the located count too, aliased per zone.

### `DXR_UNDER_SUBMIT`

| value | `PRIMARY_STEREO` | `PRIMARY_MULTIVIEW_DXR` |
|---|---|---|
| `0` strict | the located count (2) | the located count |
| `1` **default** | the located count, **or** 1 while the active mode is 1-view → accepted + one-shot `U_LOG_W` naming the fix | the located count |
| `2` kill switch | pre-ADR-041 rule | pre-ADR-041 rule (under-submit accepted) |

Out-of-range values clamp to an end, never to the default. The default flips to
`0` in the first runtime release after `displayxr-common` and the five
`displayxr-demo-*` demos ship the alias submission — the trigger is that
shipment, not a date.

CI stays on the default: a CTS session never enables `XR_DXR_display_info`, and
the deprecated arm requires it, so conformance is on the strict path at every knob
value and pinning the switch in `cts.yml` would buy nothing.

Three properties hold under all three types:

- **The reported count never moves on a mode switch.** Two counts are in play and
  only one of them moves:

  | | source | changes on a mode switch? | what it governs |
  |---|---|---|---|
  | reported view count | the begun view-configuration type | **no** | what `xrEnumerateViewConfigurationViews` / `xrLocateViews` return |
  | `active_view_count` | the active rendering mode | yes | mono-vs-3D eye assignment inside `xrLocateViews` |

  Because the reported count is immutable, the core spec rule ("the count is
  fixed by the `XrViewConfigurationType`") and
  `XR_EXT_view_configuration_views_change`'s view-count-immutability clause are
  satisfied by construction.

- **The device max still governs allocation.** `sys->view_count` keeps its old
  meaning — the device's max across modes — and is what the runtime's internal
  arrays, the IPC mirror and `xrt_device_get_view_poses` are sized to.
  `xrLocateViews` computes in device space over that max and then writes only
  the first `reported_view_count` entries out to the app, clamping
  `active_view_count` to the reported count so the eye-padding loops can never
  run past the app's array.

- **The begun type is per session.** `xrBeginSession` records
  `primaryViewConfigurationType` on the session; `xrLocateViews` with any other
  type returns `XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED`, and the
  visibility-mask path and its event follow the begun type too.

## The under-submit contract

An app under `PRIMARY_MULTIVIEW_DXR` (or a 2-view app that the workspace put into
a 4-view mode) may submit **fewer** views than the active mode has tiles. The
compositor resolves the discrepancy on the content side, per-frame:

```c
// comp_d3d11_renderer.cpp — comp_d3d11_renderer_compute_effective_layout()
views = <the projection/zone-3D layer's view_count>;
if (views > mode_tiles) views = mode_tiles;   // never the other way round
```

- `views == 1` → one tile spanning the full content region; the DP flat-blits a
  1×1 grid. This is how an always-stereo app behaves correctly in a 2D mode.
- `1 < views < mode_tiles` → the **mode's** grid, with the app painting the first
  `views` tiles (a 2-view app in a 2×2 quad mode paints tiles 0 and 1).

Same rule, same shape, on every backend:
`comp_d3d12_renderer_compute_effective_layout`,
`gl_compute_effective_layout` (`comp_gl_compositor.cpp`),
`vk_compute_effective_layout` (`comp_vk_native_compositor.c`), and the Metal
compositor's inline equivalent. Submissions are clamped to the active mode's
recipe, never the reverse — the divergence an app *wants* is expressed with the
hardware-state override (`xrRequestDisplayModeDXR`), not with a mismatched view
count ([#542](https://github.com/DisplayXR/displayxr-runtime/issues/542),
[ADR-028](../adr/ADR-028-display-mode-recipe-vs-hardware-state.md)).

Consequence: `PRIMARY_STEREO` reporting 2 costs no capability. A 2-view app in a
quad mode renders exactly what it rendered before.

**What is lost in a >2-view mode is the tiles the session cannot paint — not,
on sim-display, the symmetry of the pair.** A `PRIMARY_STEREO` session on a
device sitting in a 4-view mode receives `views[0..1]` = the first two viewer
poses of the N-view fan, and *what those two poses are is the device's business*.
On `sim_display`'s 2×2 Quad they are the symmetric half-IPD pair —
`view_eye_offsets[0..1] = {±ipd/2, eye_y, eye_z}`, with the upper row carried by
`[2..3]` at `eye_y + ipd` (`sim_display_device.c:811-815`). So the app's *eyes*
are correct; what it loses is the **unpainted upper row**, left at the clear
colour by the compositor's under-submit clamp. A device that laid its fan out
differently could hand the narrower type an asymmetric pair — the runtime does
not synthesise a centred one — but that is not what the one N-view device we
have does, and an earlier revision of this page asserted the opposite.

Both losses are what [#1499](https://github.com/DisplayXR/displayxr-runtime/issues/1499)
removes: the session is moved out of a mode it cannot fill before its first
frame ([below](#the-mode-floor-1499)). One direct consequence worth knowing
before someone debugs it cold:

- A CTS lane **forced** into quad (`SIM_DISPLAY_OUTPUT=quad`) used to fail
  `xrLocateSpace_xrLocateViews` — not on its `views.size() == 2` assertion,
  which now passes, but on its **centroid check**: VIEW space was the centre of
  the fan, while the centroid of tiles 0 and 1 is not. #1502 closes that gap
  too, because the VIEW-space offset is measured off the **reported** array
  (tiles 0 and 1 under `PRIMARY_STEREO`), not off the fan. Default CI is still
  **not** quad; see [CTS status](#cts-status). Note that #1499's floor does
  **not** rescue that lane on its own: the CTS is a LEGACY session (it enables
  no `XR_DXR_display_info`), so it is #1510's floor that applies to it, and
  #1499 is a no-op there by construction.

## The mode floor (#1499)

`PRIMARY_STEREO` reporting 2 costs no capability *as long as the display is in a
mode two views can fill*. If it is not, the session paints the first tiles and
the per-frame clear leaves the rest flat — a capability loss caused by a mode
the app may not even have chosen. So:

> **A session never runs in, or requests, a rendering mode it cannot fill.**

"Cannot fill" is `mode.view_count > <the view count of the primary view
configuration the session began>`. The rule lives in
`src/xrt/state_trackers/oxr/oxr_legacy_mode_rule.h` (named for #1510, which got
there first with a hard-coded 2) and is applied at two points:

| Point | What happens |
|---|---|
| `xrBeginSession` | If the active mode is unfillable, the display moves to one that is not — preferring a 3D mode of exactly the session's width, then any fillable 3D mode, then mode 0 (2D) — and `XrEventDataRenderingModeChangedDXR` is pushed. Apps enumerate modes *before* `xrBeginSession`, so without the event a cached `isActive` would be stale from the first frame. |
| `xrRequestDisplayRenderingModeDXR` | An unfillable request is denied with `XR_DISPLAY_MODE_DENIAL_REASON_VIEW_CONFIG_CANNOT_FILL_DXR` (`XR_DXR_display_info` v20). `XR_SUCCESS` at call time, as for every other denial; the event is the answer, and the display does not move. |

**Begin-time, not `xrGetSystem`-time** — unlike #1510's legacy twin. The view
configuration is a *per-session* fact the app names in `xrBeginSession`, so that
is the first moment `view_config_view_count` is authoritative (it is seeded from
the system's first advertised type at `xrCreateSession`). **Nothing is resized**:
the swapchain is worst-case-sized across all modes
([ADR-010](../adr/ADR-010-shared-app-iosurface-worst-case-sized.md)), so only
`recommended_view_scale_{x,y}` move.

**What an `XR_EXT_view_configuration_views_change` app sees.** The
[#1488](https://github.com/DisplayXR/displayxr-runtime/issues/1488) live-view
shadow is deliberately *not* written by the floor, so
`xrEnumerateViewConfigurationViews` keeps answering with **pre-floor** dims until
the app's **first `xrEndFrame`**, which is when the doorbell fires. Two reasons,
and the first is not the obvious one:

- the shadow is fed from the **renderer's** view dims
  (`comp_*_compositor_get_recommended_view_size`), which the renderer recomputes
  from `active_rendering_mode_index` — the mode's `view_width_pixels` through
  `u_tiling_compute_canvas_view()` — **not** from `recommended_view_scale_{x,y}`.
  Writing the scales does not feed the shadow, and writing the shadow would not
  be writing the same number the renderer will produce;
- at `xrBeginSession` a hosted or IPC-class session has no pixel dims at all (no
  window yet, no first frame), so any value written here would be a guess that
  could ring the doorbell for a size the compositor never adopts.

Leaving it alone is correct rather than merely convenient:
`oxr_views_change_seed()` baselines the edge detector on the *frozen*
`xrCreateInstance` snapshot precisely so the first sample that differs from it
counts as a change — and the first `xrEndFrame` after a floor is exactly such a
sample. The clamp, the 1 Hz throttle and the doorbell all follow from there.

### What is deliberately NOT floored

- **`PRIMARY_MULTIVIEW_DXR` sessions.** `max_views` is the device max, so the
  pick returns the active index for every mode: no floor, no denial, no warning.
  An app that wants the device's full width says so, and gets it. This is the
  invariant the whole change is built around — #1499 must not take back what
  #1486 added.
- **A session that was created but never begun.** A workspace controller drives
  the panel *on behalf of* its clients and never begins a frame loop of its own;
  it is an orchestrator, not a painter, so a painter's constraint does not apply
  to it. (This is why the denial is gated on the session running, not merely on
  the view count being known.)
- **A device that PINS its mode** (`SIM_DISPLAY_FORCE_MODE`). The pin exists to
  hold a mode against every later request — which is exactly what keeps the
  N-view under-submit path testable at all.
- **Service mode.** The panel lease, not this session, owns the display-global
  mode (ADR-035 D2). A client must not yank it from a workspace controller or
  another client.

**Those last two exempt BOTH halves, not just the floor.** A pinned session is
not floored, so it is sitting in its pinned mode — denying its request would
mean refusing it permission to re-request the mode it is already in, and the
device is the authority there anyway (it swallows the request and logs). A
service-mode client is likewise not answered locally: its request must *reach*
the lease holder, which may be moving the panel for someone else entirely.
Getting that asymmetry wrong was the first cut of this change; the two sites now
share one helper (`oxr_session_may_move_display_mode()`) so they cannot drift
apart again.

In both cases the under-submit clamp stands, and the runtime says so instead of
clamping silently: `xrBeginSession` logs `session in an UNFILLABLE rendering mode
(#1499)`, and a later display-global mode change into an unfillable mode logs
once more as it lands.

The runtime's own 1/2/3 mode keys are **not** gated, in-process or in the
service. In-process the keys only reach modes 0/1/2 (`qwerty_win32.c:514-519`),
none of which is >2-view on any device we have; in the service they are a
lease-holder action rather than a client request, so a single client that cannot
fill the result is told, not given a veto over the other clients' display.

Kill switch: `DXR_MODE_FLOOR=0` restores the pre-#1499 behaviour for extension
sessions — both halves together, and the #1499 log lines with them, since a
warning the runtime never used to print is part of what "pre-#1499" means (see
the [census](../roadmap/control-panel-performance-settings.md#test--dev--never-exposed)).
#1510's legacy floor is unaffected by it.

## Why a vendor type rather than clamping to 2

- **One worst-case swapchain** ([ADR-010](../adr/ADR-010-shared-app-iosurface-worst-case-sized.md)):
  the app swapchain is sized once for the worst case across modes and never
  resized, so a stable max-sized view surface is the matching shape for an app
  that intends to fill it.
- **`PRIMARY_STEREO` must mean two.** The core spec ties it to two views, and the
  CTS asserts it (below). A 4-view `PRIMARY_STEREO` is not a capability, it is a
  deviation that any external validation layer would reject.
- **N-view stays reachable.** Clamping everything to 2 would make a quad /
  lightfield mode unreachable through `xrLocateViews` — future-proofing the
  N-view path is the whole point of
  [#80](https://github.com/DisplayXR/displayxr-runtime/issues/80).

## CTS status

OpenXR-CTS 1.1.57 added the automated (untagged, **not** `[interactive]`) test
`xrLocateSpace_xrLocateViews`
(`src/conformance/conformance_test/test_xrLocateSpace.cpp:260`, present at tag
`openxr-cts-1.1.63.0`). For every advertised view-configuration type it asserts
that VIEW space equals the centroid of the `xrLocateViews` origins, and for
`PRIMARY_STEREO` it asserts `REQUIRE(views.size() == 2)`
(`test_xrLocateSpace.cpp:323`). That assertion is why the old model failed the
test by construction on sim-display. **With this model it passes** — verified on
the win box (sim-display, D3D11, CTS 1.1.63.0): the count assertion goes green.

The test then advances to its **centroid** assertion
(`test_xrLocateSpace.cpp:330`): VIEW space located in LOCAL must equal the mean
of the `xrLocateViews` origins. That one used to fail, deterministically
(`(0.102304, 0.043731, 0)` vs `(0, 0, 0)`, bit-identical across cold runs),
because DisplayXR's VIEW reference space was the **display plane** while the
located eyes carry the nominal-viewer / window-centre offset (ADR-012). It is a
**second, pre-existing deviation** that the count fix merely exposed — the
sim-display pair is symmetric about x=0 by construction
(`sim_display_device.c:807-815`), so the 2-view clamp cannot have shifted it.
Tracked as [#1502](https://github.com/DisplayXR/displayxr-runtime/issues/1502).

Two halves came out of that number, and only one of them was the runtime:

- **Harness**, [#1506](https://github.com/DisplayXR/displayxr-runtime/issues/1506):
  `conformance_cli.exe` embeds no DPI manifest, so on a 250 %-scaled box the
  runtime reads a 2.47×-virtualised window. Re-run under
  `__COMPAT_LAYER=HighDpiAware` and the whole x-component vanishes.
- **Runtime**, this issue: under correct DPI the residual is exactly
  `(0, 0.1025, 0)` — the nominal viewer height (`eye_y = 0.10` on sim-display)
  plus the window's own 2.5 mm off-centre. The closed form is
  `centroid − VIEW = nominal_eye − window_center_offset`, residual ~3e-5 over
  two independent window geometries.

**Fixed by #1502**: VIEW now carries a per-session **VIEW-space offset** — the
eye centroid measured off the poses `xrLocateViews` just reported — applied on
both legs of every locate (`oxr_space_ref_offset`). The head device pose is
untouched, so it stays parallax-free for the ADR-034 Amendment 2 rig source, and
`recenter` / per-app LOCAL seeding (which read `xso->semantic.view` below the
state tracker) are unaffected. See
[ADR-024 Amendment 2](../adr/ADR-024-raw-vs-render-ready-views.md#amendment-2--view-is-the-eye-centroid-2026-09-18)
and the `[view_centroid]` arms in `tests/tests_oxr_view_space.cpp`.

**Who that changes, and it is only the CTS-shaped apps.** The offset is
published only by a locate that chains no rig and is not RAW, so VIEW moves only
for sessions using **no DisplayXR view extension** — the CTS, legacy titles,
third-party OpenXR apps. Every in-tree extension app, every `displayxr-common`
consumer and the engine plug-ins chain `XR_DXR_view_rig` or are external-window
RAW, and see **no change**: measured A/B on the panel, the `cube_handle_d3d11_win`
and `cube_hosted_d3d11_win` HUD quads shift **0 px** and their text is identical.
The deliberate consequence is that two apps on one box can hold different VIEW
semantics — plane for a rig-chained or RAW app, eye centroid for a plain one;
ADR-024 Amendment 2 argues why that beats letting a rig drag VIEW.

**Confirmed on hardware** (win box, sim-display, D3D11, CTS 1.1.63.0):
`xrLocateSpace_xrLocateViews` is 9 assertions / **0 failed** (was 9/1 at `:330`);
the full D3D11 arm went 61/4 → 62 pass / 3 fail, no new reds.

**The `~xrLocateSpace_xrLocateViews` by-name exclusion has been removed** from `.github/workflows/cts.yml` and `scripts/run_cts.ps1` (follow-up to #1516): both of the test's assertions are fixed and verified (#1486: `views.size() == 2`; #1502: VIEW == centroid, 9 assertions / 0 failed on the win box), so it runs in the default lane again. Keep `__COMPAT_LAYER=HighDpiAware` or the #1506 manifest on the harness regardless — the #1502 fix is measured and so survives the DPI artefact, but every other window-geometry number the CTS sees does not.

What the CTS actually sees: it never enables `XR_DXR_display_info`, so
`PRIMARY_MULTIVIEW_DXR` is never enumerated to it. The CTS sees exactly
`PRIMARY_STEREO` with 2 views. The test's own loop iterates every enumerated
type and only **warns** on one it does not recognise, so even a future CTS run
with the extension on would not fail here.

> **Caveat, recorded as an accident and not a plan.** The CTS *conformance layer*
> exact-matches `XrViewConfigurationType` against the Khronos `xr.xml` list
> (`conformance_layer/RuntimeFailure.h:73`, `Instance.cpp:54`) and would flag any
> `DXR` value. It is moot today for two independent reasons: the CTS never
> enables `XR_DXR_display_info`, so the value is never enumerated to it; and our
> `xrSubmitDebugUtilsMessageEXT` is a stub (`oxr_api_debug.c:84-85`) while the
> loader does not fan the message out (`loader_core.cpp:617-627`), so the layer's
> flag could not reach the CTS today in any case. Neither of those is a design
> decision to lean on — a real Khronos submission of the type
> ([#80](https://github.com/DisplayXR/displayxr-runtime/issues/80)) is what
> resolves it properly.

## The kill switch — `DXR_VIEW_CONFIG_LEGACY`

`DXR_VIEW_CONFIG_LEGACY=1` restores **exactly** the pre-#1486 mapping: a single
view configuration, `PRIMARY_STEREO` for any `view_count >= 2`, reporting the max
across modes (so 4 on sim-display), and no `PRIMARY_MULTIVIEW_DXR` at all. It is
read once, in `oxr_system_fill_in()` — the only translation unit that reads it —
via `DEBUG_GET_ONCE_BOOL_OPTION`, so it is next-launch (Tier 1) and logs one
`U_LOG_W` line when active. It is registered in
[`control-panel-performance-settings.md`](../roadmap/control-panel-performance-settings.md)
Appendix A, *Test / dev — never exposed*.

It exists for one round of field bisection. It reintroduces the deviation and the
CTS failure, so a box running it is out of contract on purpose.

## What consumers see

| Consumer | Before | After | Note |
|---|---|---|---|
| **Windows browser** | latent bug | **fixed** | It calls core `xrLocateViews` with capacity **2** under `PRIMARY_STEREO` (browser-pvt `patches/0036:435`, still 2 in `0077:787`) → `XR_ERROR_SIZE_INSUFFICIENT` under sim-display. Masked on hardware only because Leia reports 2. `PRIMARY_STEREO` reporting 2 fixes it outright. It is **not** wire-only. |
| **Shell file picker** | latent bug | **fixed** | Clamps `xrEnumerateViewConfigurationViews` capacity to 2 (`file_picker_openxr.cpp:202-211`) → failed init under sim-display. |
| **Android demos** (earthview, gauss, modelviewer) | latent bug | **fixed** | All three gate on exactly 2 views and abort otherwise. |
| **Unity** | unaffected | unaffected | Stereo topology fixed at 2; truncates to 2. Stays on `PRIMARY_STEREO`. |
| **Unreal** | unaffected | unaffected **on every device that ships** | Its render loop is **N-wide** — it takes the tile count from `xrEnumerateDisplayRenderingModesDXR`, never from the view configuration — but its *content* is 2-view (views ≥ 2 duplicate the right eye). It is **not** "fixed at 2 views"; it is simply not driven by the view config. That is why it is unaffected *in practice*, not a guarantee: on a hypothetical device with a >2-view mode it would take that count from the modes list and submit `viewCount > 2` while begun on `PRIMARY_STEREO`, which `xrEndFrame` now refuses (`XR_ERROR_VALIDATION_FAILURE`). No such device ships — the only one that exists is `sim_display`'s opt-in Quad. The fix if one ever does is the same as for any N-view app: begin `PRIMARY_MULTIVIEW_DXR` (`INV-3.1`). |
| **DisplayXR extension apps** | correct | opt in | `INV-3.1` ([app rules](../guides/displayxr-app-rules.md)) now reads: an N-view app begins with `PRIMARY_MULTIVIEW_DXR` when enumerated; a stereo-fixed app stays on `PRIMARY_STEREO` and gets 2. |
| **Legacy apps** hardcoding 2 | out of contract | correct | They were only ever broken by the max-across-modes count; `PRIMARY_STEREO` = 2 makes them right. |

**Nothing in-tree actually renders more than 2 views of content.** The only
device that ever sets `view_count > 2` is `sim_display`, whose Quad mode is
opt-in (`SIM_DISPLAY_OUTPUT=quad` / `SIM_DISPLAY_FORCE_MODE=4`) and self-described
as a *"view-inspection tool, not a sane default"*
(`sim_display_device.c:52-56`). The Leia plug-in hardcodes
`hmd->base.hmd->view_count = 2` on every platform (`src/drv_leia/leia_device.c:257`)
and declares two modes, 2D (1 view) and LeiaSR (2 views). So
`PRIMARY_MULTIVIEW_DXR` is future-proofing plus sim-display CI conformance — it
is not preserving a shipping N-view product today.

## History — what the deviation was

Before this change the runtime modelled **one** view configuration per system
(`oxr_system.c`, a single `sys->view_config_type`):

```c
sys->view_count = view_count;
if (view_count == 1) {
        sys->view_config_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
} else {
        // view_count >= 2: treat as stereo (including quad, lightfield, etc.)
        sys->view_config_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
}
```

`sys->view_count` — the **max across the device's rendering modes**, computed in
`sim_display_device.c` — was handed verbatim to
`xrEnumerateViewConfigurationViews` and `xrLocateViews`. `sim_display` declares 5
modes (2D 1 view, Anaglyph 2, Cropped SBS 2, Squeezed SBS 2, Quad 4), so **every**
sim-display instance reported **4 views for `PRIMARY_STEREO`**, in every mode.
That was the deviation: not latent, not mode-dependent, and in direct conflict
with the spec's two-view definition of `PRIMARY_STEREO`.

It was recorded (rather than fixed) in
[#1491](https://github.com/DisplayXR/displayxr-runtime/pull/1491), which also
added the named CTS exclusion this change removes. Three claims in that first
write-up were wrong and are corrected above: quad was called *"a shipping
capability of the display class"* (it is a sim-display dev-only inspection mode),
the browser was called *wire-only* (the Windows browser calls core
`xrLocateViews`), and Unreal was called *"fixed at 2 views"* (its loop is N-wide;
only its content is 2-view).
