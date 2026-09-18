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
| `xrEndFrame` projection `viewCount` accepted | 1 | **2** (1 only in a 1-view mode) | 1, 2, or any rendering mode's `viewCount` |
| Fixed for the instance lifetime? | yes | yes | yes |

> **The one-view exception is scoped to 1-view modes, and that is a CTS
> requirement, not a preference.** `PRIMARY_STEREO` accepts `viewCount == 1` only
> when the **active rendering mode** is itself 1-view — the 2D/mono submission
> path the extension has always allowed (`cube_*` apps compute
> `eyeCount = display3D ? modeViewCount : 1`; `displayxr-common` forwards the
> caller's count). In a 2-view mode a short submission is
> `XR_ERROR_VALIDATION_FAILURE`, because the CTS
> `XrCompositionLayerProjection` test decrements the located count
> (`test_XrCompositionLayerProjection.cpp:225-230`: `Layer.viewCount--`, then
> `CHECK(XR_ERROR_VALIDATION_FAILURE == endFrame(...))`) and the CTS runs in a
> 2-view mode. An unconditional "1 or 2" turned that required rejection into a
> success. This is the only place the `xrEndFrame` rule consults the active mode;
> the *reported* counts above still never move on a mode switch.

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

**What the two views ARE in a >2-view mode, though, is tiles 0 and 1 — not a
symmetric stereo pair.** A `PRIMARY_STEREO` session on a device sitting in a
4-view mode receives `views[0..1]` = the first two viewer poses of the N-view
fan, which for a 2×2 quad are two adjacent slots off to one side of the viewer,
not a left/right pair straddling it. The runtime does not synthesise a
centred pair for the narrower type. This is dev-only today — `sim_display`'s
Quad is opt-in and no shipping device exceeds 2 views — and
[#1499](https://github.com/DisplayXR/displayxr-runtime/issues/1499) tracks
suppressing the mode switch for a session that cannot express it. Two direct
consequences worth knowing before someone debugs them cold:

- A CTS lane **forced** into quad (`SIM_DISPLAY_OUTPUT=quad`) would fail
  `xrLocateSpace_xrLocateViews` — not on its `views.size() == 2` assertion,
  which now passes, but on its **centroid check**: VIEW space is the centre of
  the fan, while the centroid of tiles 0 and 1 is not. Default CI is **not**
  quad, so the lane is green; see [CTS status](#cts-status).
- A shipped 2-view app the workspace pushes into a wider mode gets asymmetric
  eyes for the duration. Same root cause, same fix in #1499.

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
of the `xrLocateViews` origins. That one fails, deterministically
(`(0.102304, 0.043731, 0)` vs `(0, 0, 0)`, bit-identical across cold runs),
because DisplayXR's VIEW reference space is the viewer origin while the located
eyes carry an offset (the `#1370` test in `tests/tests_oxr_view_space.cpp`
describes exactly that "eye-centroid offset from the VIEW origin"). This is a
**second, pre-existing deviation** that the count fix merely exposed — the
sim-display pair is symmetric about x=0 by construction
(`sim_display_device.c:807-815`), so the 2-view clamp cannot have shifted it.
Tracked as [#1502](https://github.com/DisplayXR/displayxr-runtime/issues/1502).

**So the `~xrLocateSpace_xrLocateViews` by-name exclusion stays** in
`.github/workflows/cts.yml` and `scripts/run_cts.ps1`, re-pointed at #1502: a
known, named red rather than an unnamed one. The view-count half is fixed; the
centroid half is tracked. Delete the exclusion when #1502 lands.

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
