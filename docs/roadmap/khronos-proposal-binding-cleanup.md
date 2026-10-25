# Cleanup plan: the extension set we propose to the OpenXR WG

**Status:** plan, no code yet (2026-10-25). Tracks the prep work for one
`KhronosGroup/OpenXR-Docs` issue that proposes a spatial-display extension set
for Working Group direction. The issue will reference the number-reservation PR
[OpenXR-Docs#206](https://github.com/KhronosGroup/OpenXR-Docs/pull/206) (open,
retargeted to `staging` 2026-10-01, mirrored as KHR GitLab openxr#2822).

The issue asks the WG two things: (a) whether we can publish these as `XR_DXR_*`
vendor extensions in the official registry, and (b) whether there is appetite
for a multi-vendor `XR_EXT_*` version.

**Hard constraint:** nothing in this plan may regress a shipped app, engine
plug-in or demo. The July 2026 `XR_EXT_*` → `XR_DXR_*` rename shipped without
aliasing, on purpose. That was a one-off, and this plan does not repeat it.

## 1. The set we propose: four questions a display answers

| Question | Extensions | In the issue |
|---|---|---|
| 1. Where does the content go? | `XR_DXR_win32_window_binding`, `_cocoa_window_binding`, `_xlib_window_binding`, `_wayland_surface_binding`, `_android_surface_binding`, plus the window-space composition layer | yes. Analogy for the WG: `XR_KHR_D3D11_enable` and friends say which API you render with; window bindings say where the result is presented. App-owned windows are the main thing DisplayXR adds. |
| 2. What is the display? | `XR_DXR_display_info` (core subset only, see §3.4) | yes |
| 3. How do I render for this viewer? | `XR_DXR_view_rig` | yes |
| 4. Which part is 3D? | `XR_DXR_local_3d_zone`, `XR_DXR_display_zones` | yes |

**Left out of the issue:** `spatial_workspace`, `desktop_control`, `mcp_tools`,
`lift`, `stereo_camera`, `cursor_depth`, `workspace_file_dialog`,
`input_providers`, `depth_budget`, `atlas_capture`, `weave`, `macos_gl_binding`.
`weave` (the present-owner route the browser uses) and `macos_gl_binding` (a
graphics binding, not a window binding) are the two closest to the set. The
issue can mention them in one line as future work.

## 2. What the headers actually say (verified 2026-10-25 against `main` e5d7ffa57)

| # | Finding | Kind |
|---|---|---|
| F1 | **Cocoa does have a session create-info**: `XrCocoaWindowBindingCreateInfoDXR` (`viewHandle` = an `NSView*` backed by a `CAMetalLayer`, plus `readbackCallback`, `sharedIOSurface` and `transparentBackgroundEnabled`). It also has `XrCocoaWindowPlacementInfoDXR` (v8, ADR-050). So all five bindings have the same shape: a `*BindingCreateInfoDXR` chained on `XrSessionCreateInfo`. The real per-platform differences are the **live-update calls**: Wayland has `xrSetWaylandSurfaceGeometryDXR`, Android has `xrSetAndroidSurfaceDXR`, `xrSetAndroidWindowGeometryDXR`, `xrSubmitAndroidInputEventDXR` and a layout-hint event, and cocoa has the placement-ownership struct. The runtime `CLAUDE.md` says cocoa passes an "NSWindow"; it actually passes an NSView. | doc drift |
| F2 | **`XrCompositionLayerWindowSpaceDXR` is defined in two headers** (win32 and cocoa) with the same value `1004999002`. That is the only duplicated `XrStructureType` value among all DXR headers. Cocoa wraps its copy in `#ifndef XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR`; win32 does not. So a TU that includes cocoa **before** win32 fails to compile with a struct redefinition. `PFN_xrReadbackCallback` is also typedef'd in both headers without a guard (legal only because the two typedefs are identical). | header hygiene |
| F3 | **No extension owns the window-space layer on Linux or Android.** `verify_window_space_layer` (`oxr_session_frame_end.c:1429`) accepts the layer on any native-compositor, IPC or external-window session, and never checks which extension was enabled. On xlib, Wayland and Android, no advertised extension even declares the type. `displayxr-common`, the demos, Unity and the Unity samples submit it on every platform. The WG will ask which extension owns the struct, and right now the answer is "win32 or cocoa, sort of". | spec gap |
| F4 | The win32 create-info has a dead `#ifdef _WIN32`: both branches declare the same `void* windowHandle`. | cosmetic |
| F5 | **`readbackCallback` is a C function pointer inside a create-info** (win32 and cocoa). That is unusual for OpenXR, and the WG will push back. No shipped consumer passes a non-NULL value: Unity, the browser and the demos all set it to NULL. The only other user is a separate legacy runtime fork. **`sharedTextureHandle` / `sharedIOSurface` are live**: the texture class is used by the browser, `displayxr-cef-host` and the Unity provider (a probe handle). | design question |
| F6 | Stale spec text. win32 says "Platform: … Linux/Android reserved for future use" (line 9) and has a "Linux / Android platform support" future item (line 968). Cocoa says Android is "planned, not yet implemented" (line 258), but it shipped in #1037. xlib says "Spec Version 1" with only a v1 history row, while the header is at **v2**. xlib §6 still lists `transparentBackgroundEnabled` as not wired, but the v2 header carries the field. `display_info.md` says "Status: Draft — proposed for Khronos OpenXR Working Group review". | doc drift |
| F7 | Inconsistent authorship. The `Author` field says `Leia Inc.` in win32, cocoa and display_zones, `David Fattal (Leia Inc.)` in display_info, and `The DisplayXR Project` in xlib and wayland. Every header's copyright line says `The DisplayXR Project`, and so does the #206 reservation. | doc drift |
| F8 | **`XR_DXR_display_info` is at SPEC_VERSION 23** (not 15). It now covers five concerns: core display facts (`XrDisplayInfoDXR`); rendering modes (`xrEnumerateDisplayRenderingModesDXR`, `xrRequestDisplayRenderingModeDXR`, mode-changed, hardware-state and request-denied events); eye-tracking modes (`xrRequestEyeTrackingModeDXR`, capabilities, per-view state, state-changed event); desktop placement (`XrDisplayDesktopPositionDXR`, `XrDisplayDesktopInfoDXR`); and multi-display (`XrDisplayDXR`, `xrEnumerateDisplaysDXR`, display spaces, session/view display bindings, displays-changed event), plus `XrViewActivityStateDXR`. | scope |
| F9 | **Every type value is provisional (`1004999xxx`).** #206 reserves extension numbers 1011–1026 (plus placeholders 1027–1034). By OpenXR convention, number *n* owns enum values `1000000000 + (n−1)×1000 + k`, so `display_info` (1015) would own `1001014000+`. Moving any struct to its official value breaks the ABI for every binary built against today's headers. **Several consumers hand-copy the values** instead of including the headers. For example, the Unity provider's `native~/displayxr_extensions.h` hard-codes `1004999001/003/008/010/011/013/210/211/212`. `GOVERNANCE.md` (in `displayxr-extensions`) says the values are "not stable" but defines no migration. This is the largest regression risk in the whole effort. | ABI policy |
| F10 | The headers point at `GOVERNANCE.md`, which exists only in the `displayxr-extensions` mirror. The pointer resolves in the mirror and dangles in this tree. | cosmetic |

## 3. Consumers: who breaks if a struct changes

From a source grep of the sibling checkouts (worktrees de-duplicated):

| Struct / entry point | Consumers outside the runtime |
|---|---|
| `XrWin32WindowBindingCreateInfoDXR` | Unity (9 files), Unity samples, Unreal, browser, cef-host, leia-plugin, shell, all five demos, XRInteractiveVideoPlayer |
| `sharedTextureHandle` | browser, cef-host, Unity, Unreal, leia-plugin |
| `readbackCallback` (non-NULL) | **none** shipped (a legacy runtime fork only) |
| `XrCocoaWindowBindingCreateInfoDXR` / `sharedIOSurface` | Unity, Unity samples, browser (mac arm), all five demos |
| `XrCocoaWindowPlacementInfoDXR` | runtime only |
| `XrXlibWindowBindingCreateInfoDXR` | Unity, mediaplayer |
| `XrWaylandSurfaceBindingCreateInfoDXR` | Unity (Linux) |
| `XrAndroidSurfaceBindingCreateInfoDXR` | avatar, mediaplayer, Unity Android branch |
| `XrCompositionLayerWindowSpaceDXR` | displayxr-common, Unity (15), Unity samples (11), browser, all five demos, XRInteractiveVideoPlayer |
| `XrDisplayInfoDXR` | browser (8), Unity, Unreal, shell, web, cef-host, all five demos, XRInteractiveVideoPlayer |
| `XrDisplayDesktopPositionDXR` | browser, Unity, all five demos |
| `xrRequestDisplayModeDXR` | browser (12), leia-plugin, Unity, Unreal, displayxr-common, four demos |
| `xrRequestEyeTrackingModeDXR` | browser, Unity, displayxr-common, four demos, XRInteractiveVideoPlayer |
| `xrEnumerateDisplaysDXR` | runtime only |
| `XrDisplayRigDXR` / `XrCameraRigDXR` | displayxr-common, Unity, Unreal, all five demos |
| `XrCompositionLayerLocal2DDXR` | displayxr-common, Unity, avatar, mediaplayer, modelviewer |
| `XrDisplayZonesFrameEndInfoDXR` | Unity, avatar |

Conclusion: every struct in the proposed set except `xrEnumerateDisplaysDXR`
and the cocoa placement info is in shipped binaries. **Every change to a
proposed struct's layout, name or type value counts as breaking.** Only
additive changes (new structs, new chained structs, new extensions) are safe.

## 4. Staged plan

Each stage lists the proof that it regressed nothing. A stage does not start
until the previous one has merged.

### Stage 0: docs only (before posting the issue). Zero ABI risk.

1. Fix F6: rewrite the win32 `Platform` line, delete the stale Linux/Android
   future item, fix cocoa's Android line, bring xlib's version line and history
   up to v2, update its §6, and change display_info's `Status` line to
   "Implemented; provisional values".
2. Fix F7: set `Author` to **The DisplayXR Project** everywhere (§6, D5), with
   a `Contributors` line naming people. Leia incubates the project; the README
   and GOVERNANCE say so once, so the specs don't have to repeat it.
3. Fix F1 in the runtime `CLAUDE.md`: cocoa passes an NSView, not an NSWindow.
4. Write `docs/specs/extensions/spatial-display-set.md`: a one-page overview
   built on the four questions, with the core subset of each extension and the
   open questions from §5. The issue links to this page instead of to 9 specs
   that are each 300–900 lines.
5. Hands off two areas that belong to ADR-060 / win32 v9 (§7): the
   `sharedTextureHandle` / `windowHandle` / `readbackCallback` semantics in
   win32 §3.2, and the "zone rects are client-window px" wording in
   display_zones.

Proof: the `lint.yml` `gen_extensions_index.py --check` run and doc-only CI.

### Stage 1: header hygiene (optional before posting). Source-compatible, ABI-identical.

1. **First, add an ABI snapshot test.** Add a C test (`tests/` + ctest) that
   pins, for every struct in the proposed set, `sizeof`, `offsetof` of every
   member, every `XR_TYPE_*_DXR` value and every `*_SPEC_VERSION`. Generate it
   from today's headers and commit it red-green. Every later stage has to keep
   it green, or bump it deliberately with an additive change. This test is
   what makes "no regression" checkable.
2. Fix F2: move `XrCompositionLayerWindowSpaceDXR`, its type define and
   `PFN_xrReadbackCallback` into one guarded block that both headers share
   (either an internal `XR_DXR_window_space_layer_types.h` that both include,
   or the cocoa-style `#ifndef` copied into win32). Names and values stay
   exactly the same.
3. Fix F4: remove the dead `#ifdef`. Fix F10: point the headers at the mirror
   URL instead of a bare filename.

Proof: the ABI snapshot test; Windows, macOS, Linux and Android CI;
`build-mingw-check.sh`; the `displayxr-extensions` mirror sync; and one rebuild
of each header-vendoring consumer against the new headers (Unity native,
Unreal, displayxr-common, the five demos) to catch include-order breakage. No
runtime behavior changes, so no panel run is needed.
**Coordinate first:** this touches `XR_DXR_win32_window_binding.h`, so confirm
with the ADR-060 session (§7) and land it before their v9, not after.

### Stage 2: additive spec changes (after WG feedback, not before posting)

Each item is additive: a new extension or a new chained struct. No existing
struct is moved, renamed or renumbered, and the runtime keeps accepting the
current form indefinitely.

1. **Window-space layer gets its own extension (D2).** Add
   `XR_DXR_window_space_layer` (number from the 1027–1034 pool) that owns the
   struct and **keeps the value `1004999002`**. The runtime keeps accepting the
   layer exactly as today, so an app that never enables the new extension
   still works. The first time such an app submits the layer, the runtime logs
   one WARN. Proof: the ABI test; `cube_*` window-space submits per platform;
   the Unity samples and demos that submit the layer (avatar, mediaplayer);
   and one **previously released** demo binary (installed from an old
   release) running against the new runtime.
2. **readbackCallback (D1).** The spec marks it *deprecated, must be NULL in
   new code*. The field stays where it is, and the runtime keeps honoring it.
   Removing it would change the struct's layout.
3. **display_info core vs plumbing (D4).** Do not split the existing extension.
   If the WG wants a smaller core, the multi-vendor `XR_EXT_*` version defines
   it fresh, and the DXR extension stays as it is.

### Stage 3: official numeric values (only after #206 merges and the WG decides)

This is the one breaking change, and it needs a policy before anyone writes
code (D3). The direction we propose:
- **App → runtime structs**: the runtime switches on `type` and accepts
  **both** the provisional and the official value for a published deprecation
  window. This is pure addition in the state tracker.
- **Runtime → app structs** (events, `xrGetSystemProperties` outputs the
  runtime fills in): the runtime cannot know which header an app was built
  against. So it keeps writing provisional values until the app opts in. The
  opt-in could be a bumped `SPEC_VERSION` that the app requests, or a marker
  extension. Ask the WG which mechanism other vendors have used.
- Consumers that hand-copy the values (Unity's `displayxr_extensions.h` first)
  switch to including the published headers **before** the values change.
  That is a prerequisite PR in each consumer repo.
Proof: the ABI test with both value sets; full CTS (#1523 matrix); released
binaries of Unity, the browser, the demos, Unreal and the shell running
against the new runtime; and real-panel runs on Windows (Leia) and macOS,
because event delivery drives 2D/3D switching.

## 5. Order relative to the issue

**Must land before we post:** Stage 0 (docs only, small). Stage 1 is
recommended, because a reviewer who includes both headers hits F2 right away,
but it is not required.

**Listed in the issue as known open design questions, not fixed first:**
1. The texture class (`sharedTextureHandle` / `sharedIOSurface`) as a
   create-info field vs. its own binding struct (ADR-060 is reworking it now).
2. `readbackCallback`: we plan to deprecate it, and we ask whether the WG would
   rather see it removed in an EXT version.
3. Who owns the window-space layer: a separate extension (our proposal).
4. How to migrate from provisional to official values (F9, Stage 3). Ask the WG
   directly.
5. The scope of display_info: core facts and rendering modes vs. eye-tracking
   modes, desktop placement and multi-display.
6. Whether `XR_DXR_weave` (an app that presents for itself) belongs with
   question 1.

## 6. Decisions for David

| | Decision | Recommendation |
|---|---|---|
| D1 | Texture class + readback: keep both in the binding and flag them, or split them out now | Keep both, flag in the issue, deprecate readback in docs. Don't split while ADR-060 v9 is in flight. |
| D2 | Window-space layer: its own extension, or leave it in win32 + cocoa | Its own extension, additive, value unchanged |
| D3 | Provisional → official values policy | Dual-accept inbound, opt-in outbound; ask the WG before writing code |
| D4 | display_info core subset for the issue | `XrDisplayInfoDXR` + rendering modes + mode/hardware-state events. Out: desktop position/info, eye-tracking mode request, multi-display (mention as future work) |
| D5 | Author naming | "The DisplayXR Project" (matches copyrights and #206) |
| D6 | Doc vs issue for this plan | This doc on a draft PR; flip it to a tracking issue if you prefer checkboxes |

## 7. Coordination with ADR-060 (`#697`, texture target binding)

The ADR-060 session (local branches `docs/adr-697-texture-target-binding`
@010c2ff9c and `feat/r2-weave-windowless-phase` @571aa2f6e, neither pushed yet;
no PR until David says so, and not this week) plans:
- `XR_DXR_win32_window_binding` **SPEC_VERSION 9**, additive only (approved by
  David 2026-10-25). It adds `XrWin32TextureTargetBindingDXR`,
  `XrWin32TextureTargetFrameDXR`, `XrSystemWin32TextureTargetPropertiesDXR` and
  `XrWin32TextureTargetStateDXR`, using provisional values
  **1004999015–018**. These are free today: the 0xx block currently stops at
  014, and 009 is unused.
- Existing fields keep their meaning. Under a target binding, `windowHandle`
  becomes the fallback anchor and `readbackCallback` must be NULL.
- display_zones rects become target-local under a target binding, with no
  header change. Cocoa gets the same pair later. The `XR_DXR_weave` change is
  a comment only.
- Not touched: display_info, view_rig, local_3d_zone and the window-space
  layer.

What this plan therefore leaves alone: the win32 v8→v9 numbering, values
015–018, the win32 §3.2 texture-class semantics, and the display_zones rect
wording. Stage 1's win32 edit is editorial and type-preserving, but we still
tell that session before it lands, so their v9 builds on it. Any rename or
deprecation in win32, the texture class or zones (D1, D2) goes to them first.
