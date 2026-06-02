---
status: Proposal
owner: David Fattal
updated: 2026-06-02
issues: []
code-paths:
  - src/xrt/state_trackers/oxr/oxr_workspace.c
  - src/xrt/auxiliary/util/u_capture_intent.h
  - src/external/openxr_includes/openxr/XR_EXT_spatial_workspace.h
  - test_apps/common/atlas_capture*
---

> **Status: Proposal** — not yet implemented. Promote the "Decision" section to
> an ADR (next free number) once accepted.

# Unified Atlas Capture API

One official runtime entry point for "snapshot the multi-view atlas to a PNG,"
callable by **every** app class and every graphics API, replacing the five
independent capture implementations that exist today.

## Problem

A capture feature (almost always bound to the **`I`** key; `Ctrl+Shift+C` in the
shell) is reimplemented in every consumer. Each one does its own GPU→CPU
readback against its **own** swapchain, before submit:

| Consumer | Trigger | Capture impl | Notes |
|---|---|---|---|
| Test apps (`cube_handle_*`) | `I` | `test_apps/common/atlas_capture_{d3d11,d3d12,gl,vk,metal}` | canonical helper (8 files) |
| Gauss demo | `I` | forked `common/atlas_capture*` (VK only) | copy of the helper |
| Model-viewer demo | `I` | forked `common/atlas_capture*` (VK only) | copy of the helper |
| Unity plugin | `I` | `native~/displayxr_readback.*` + `DisplayXRScreenshot.cs` (`AsyncGPUReadback`) | engine-native re-port |
| Unreal plugin | `I` / `DisplayXR.CaptureAtlas` / Blueprint | `DisplayXRAtlasCapture.cpp` (RHI `ReadSurfaceData`) | engine-native re-port |
| Shell (workspace) | `Ctrl+Shift+C` | **`xrCaptureWorkspaceFrameEXT`** (IPC → `comp_d3d11_service_capture_frame`) | already a runtime API |

Two structural problems:

1. **Duplication.** ~5 per-API readback paths × 3 C++ source copies
   (`test_apps/common` + two demo forks) + 2 engine-native re-ports. Every new
   vendor demo forks the helper again.
2. **Wrong capture point.** Apps can only read their **own pre-submit swapchain**
   — projection content only. They physically cannot see runtime-composed
   window-space (HUD) / quad layers, cursor, or workspace chrome. There is no
   way for an app to get "what the display processor actually saw."

Meanwhile the runtime **already owns** everything needed to do this centrally —
it just isn't exposed as one public call.

## What the runtime already has

Two runtime-side capture capabilities exist today; neither is the unified API,
but together they are ~90% of the implementation.

1. **`u_capture_intent`** (`src/xrt/auxiliary/util/u_capture_intent.h`) — present
   in every in-process compositor (d3d11, d3d12, gl, vk_native; metal partial).
   It already has the two-mode distinction we want:
   - `MCP_CAPTURE_MODE_POST_COMPOSE` (0) — atlas handed to the DP (projection +
     window-space + quads), end of frame.
   - `MCP_CAPTURE_MODE_PROJECTION_ONLY` (1) — atlas with only projection-class
     layers, captured at the projection-done boundary.

   Driven today by **dev trigger files** (`%TEMP%\displayxr_atlas_trigger[.projection]`)
   and the **MCP `capture_frame` tool** (`mcp_capture_blocking_handler`, which
   submits a request and blocks up to 3 s for the compositor thread to do the
   readback + PNG encode). Per-API readback lives in each compositor's
   `*_capture_atlas_to_png`.

2. **`xrCaptureWorkspaceFrameEXT`** (`XR_EXT_spatial_workspace`, spec_version 5+)
   — a real OpenXR extension function, but **privileged** (workspace controller
   only), **IPC/service-mode only**, **D3D11 only**, and **post-compose only**
   (`XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT`, no mode flag). The shell uses it.
   It returns rich metadata (atlas/eye dims, tile layout, eye poses) that apps
   currently reconstruct by hand.

The gap is purely **plumbing**: expose a non-privileged, all-session,
all-API, mode-flagged entry point on top of (1), and let the shell's existing
privileged call share the same readback core.

## Decision

Add a new vendor-neutral extension **`XR_EXT_atlas_capture`** with a single
function `xrCaptureAtlasEXT`. It captures the atlas the runtime composes **for
the calling session**, at a caller-selected stage (`PROJECTION_ONLY` /
`POST_COMPOSE`), and returns the same metadata block the workspace capture
returns.

- **Any session** may call it (handle / texture / hosted / IPC). For in-process
  sessions the oxr entry point drives the in-process compositor's
  `u_capture_intent` via the existing `mcp_capture` hand-off; for IPC sessions it
  routes over the existing workspace-capture IPC bridge.
- **All graphics APIs** are covered because the runtime does the readback with
  the compositor's own `*_capture_atlas_to_png` — the app never touches a
  staging texture.
- **`xrCaptureWorkspaceFrameEXT` stays** as the *privileged, cross-client*
  capture (the whole workspace composite, all clients — inherently a workspace
  concern). It is reimplemented on the shared readback core and **gains the mode
  flag** (rule 2 below). Apps move to `xrCaptureAtlasEXT`; the shell keeps the
  workspace call.

Rationale for a **new** extension rather than ungating the workspace one: a
general all-apps capture API must not live behind a "workspace controller"
privilege or carry `Workspace` in its name — the workspace extension is a
customer-facing privileged surface, not the home for a universal app feature
(consistent with the no-`shell`-in-runtime-vocabulary boundary). The *unification*
is at the runtime readback layer, which both extensions share.

## Extension delta

New header `src/external/openxr_includes/openxr/XR_EXT_atlas_capture.h`
(auto-synced to `displayxr-extensions` like the others):

```c
#define XR_EXT_atlas_capture 1
#define XR_EXT_atlas_capture_SPEC_VERSION 1
#define XR_EXT_ATLAS_CAPTURE_EXTENSION_NAME "XR_EXT_atlas_capture"

// Reuse the reserved 1000999xxx range (next free slot after the workspace block).
#define XR_TYPE_ATLAS_CAPTURE_INFO_EXT   ((XrStructureType)1000999120)
#define XR_TYPE_ATLAS_CAPTURE_RESULT_EXT ((XrStructureType)1000999121)

#define XR_ATLAS_CAPTURE_PATH_MAX_EXT 256

typedef enum XrAtlasCaptureStageEXT {
    XR_ATLAS_CAPTURE_STAGE_POST_COMPOSE_EXT   = 0, // DP-bound atlas (proj + window-space + quads)
    XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT = 1, // projection-class layers only
    XR_ATLAS_CAPTURE_STAGE_MAX_ENUM_EXT        = 0x7FFFFFFF
} XrAtlasCaptureStageEXT;
// NB: values match enum mcp_capture_mode so no translation layer is needed.

typedef struct XrAtlasCaptureInfoEXT {
    XrStructureType        type;   // XR_TYPE_ATLAS_CAPTURE_INFO_EXT
    const void* XR_MAY_ALIAS next;
    XrAtlasCaptureStageEXT  stage;
    char                    pathPrefix[XR_ATLAS_CAPTURE_PATH_MAX_EXT]; // runtime appends "_atlas.png"
} XrAtlasCaptureInfoEXT;

// Identical metadata block to XrWorkspaceCaptureResultEXT (see rationale below).
typedef struct XrAtlasCaptureResultEXT {
    XrStructureType type;          // XR_TYPE_ATLAS_CAPTURE_RESULT_EXT
    void* XR_MAY_ALIAS next;
    uint64_t        timestampNs;
    uint32_t        atlasWidth, atlasHeight;
    uint32_t        eyeWidth, eyeHeight;
    uint32_t        tileColumns, tileRows;
    float           displayWidthM, displayHeightM;
    float           eyeLeftM[3], eyeRightM[3];
} XrAtlasCaptureResultEXT;

typedef XrResult (XRAPI_PTR *PFN_xrCaptureAtlasEXT)(
    XrSession                    session,
    const XrAtlasCaptureInfoEXT *info,
    XrAtlasCaptureResultEXT     *result); // result may be NULL if the caller wants only the PNG
```

Edit to `XR_EXT_spatial_workspace.h` (spec_version bump): add the second capture
flag so the privileged path also supports stage selection.

```c
// spec_version <next>: select projection-only vs post-compose (default).
static const XrWorkspaceCaptureFlagsEXT XR_WORKSPACE_CAPTURE_FLAG_PROJECTION_ONLY_BIT_EXT = 0x00000002u;
```

## Runtime wiring (no new GPU code)

`xrCaptureAtlasEXT` → `oxr_xrCaptureAtlasEXT` (new, alongside
`oxr_xrCaptureWorkspaceFrameEXT` in `oxr_workspace.c` or a new `oxr_capture.c`):

1. Validate `info->type`, extension-enabled, `info->stage` in range.
2. **Branch on session compositor type** (the inverse of the current IPC gate in
   `oxr_workspace.c`):
   - **In-process** (`comp_d3d11/d3d12/gl/vk_native/metal`): call
     `mcp_capture_blocking_handler(info->pathPrefix-with-suffix, stage, NULL)`.
     That submits to the same `u_capture_intent` the trigger files and MCP tool
     already use, and blocks until the compositor thread completes the readback.
     Metadata for the result struct comes from the compositor's atlas/tile state
     (the same values `mcp_capture` already knows).
   - **IPC** (`comp_ipc_client`): route through
     `comp_ipc_client_compositor_workspace_capture_frame` (already exists) with
     the stage mapped to the capture flags; the service-side
     `comp_d3d11_service_capture_frame` honors the new
     `PROJECTION_ONLY` bit.
3. Register in `oxr_api_negotiate.c` / `oxr_api_funcs.h`; add
   `OXR_HAVE_EXT_atlas_capture` gating in `oxr_extension_support.h`.

The metadata struct is byte-identical in layout to
`XrWorkspaceCaptureResultEXT` minus the `viewsWritten` field; the service path
already fills all of it, and `mcp_capture` can fill it for the in-process path.

## Per-repo deletion list

What collapses once apps call `xrCaptureAtlasEXT` + a ~10-line key handler.

**`openxr-3d-display` (this repo)** — `test_apps/common/`:
- DELETE the per-API readback TUs: `atlas_capture_d3d11.cpp`,
  `atlas_capture_d3d12.cpp`, `atlas_capture_gl.cpp`, `atlas_capture_vk.cpp`,
  `atlas_capture_metal.mm`.
- KEEP (shrunk): `atlas_capture.{h,cpp}` / `atlas_capture_macos.mm` — only the
  **flash overlay** + filename-numbering UX is app-side (window/AppKit-specific).
  `MakeCapturePath` / `NextCaptureNum` may move into the runtime so the result
  metadata reports the final path.
- Each `cube_handle_*` main loop drops its ~30–40 lines of swapchain-dims +
  `CaptureAtlasRegion*` glue, replaced by one `xrCaptureAtlasEXT` call. The `I`
  key handler in `test_apps/common/input_handler.cpp` is unchanged.

**`displayxr-demo-gaussiansplat`** — `common/`:
- DELETE `atlas_capture.cpp`, `atlas_capture.h`, `atlas_capture_macos.mm`,
  `atlas_capture_vk.cpp` (the entire forked helper).
- Replace the call site in `windows/main.cpp` / `macos/main.mm` with
  `xrCaptureAtlasEXT`.

**`displayxr-demo-modelviewer`** — `common/`:
- DELETE `atlas_capture.cpp`, `atlas_capture.h`, `atlas_capture_macos.mm`,
  `atlas_capture_vk.cpp` (identical fork).
- Replace the call site in `windows/main.cpp`.

**`unity-3d-display`** (see "Engine-plugin specifics" below — the engine case is
materially different from native apps):
- GUT the readback in `Runtime/DisplayXRScreenshot.cs` — for a **live OpenXR
  session**, `BeginCapture` (`AsyncGPUReadback` + PNG encode) and the entire
  on-demand re-render path (`CaptureOnDemand`, the hidden `s_CaptureCam` rig,
  `GetStereoMatrices`, `FlipViewZ`) are replaced by a P/Invoke to
  `xrCaptureAtlasEXT` via the plugin's existing extension dispatch. This deletes
  the most fragile code in the file (manual Kooima re-render of L/R views).
- KEEP: the flash overlay (`DisplayXRFlashOverlay`, `_DrawFlashGL`), filename
  numbering (`NextSequenceNumber`), and the `I`-key binding (which lives in the
  **sample** `Samples~/.../DisplayXRInputController.cs`, mirrored into the three
  `displayxr-unity-test*` repos — repoint, don't delete).
- DO **NOT** touch `native~/displayxr_readback.{cpp,h}` — that is the macOS
  **offscreen-preview** readback buffer, unrelated to screenshots. (My first
  draft wrongly listed it.)
- OUT OF SCOPE: the **editor preview-session** capture path (`_OnAtlasReady`,
  gated on `displayxr_standalone_is_running()`). That path renders in-editor
  **without a DisplayXR OpenXR runtime session**, so there is no runtime
  compositor to call `xrCaptureAtlasEXT` on. It keeps its own atlas-RT readback
  (or we accept no capture in pure-editor preview).

**`displayxr-unreal`** — canonical source is
`Source/DisplayXRCore/Private/DisplayXRAtlasCapture.{cpp,h}`. The identical copy
under `Packages/DisplayXR_5.7/Source/...` is the **packaged/zipped distributable**
plugin (regenerated from `Source/`, also present as `DisplayXR_5.7.zip`) — don't
hand-edit it; it falls out when `Source/` is repackaged.
- DELETE the RHI `ReadSurfaceData` body of `ProcessRequest_RenderThread` and the
  render-thread plumbing that feeds it the engine atlas RT. Keep `RequestCapture`
  (the arm), the `I` key / `DisplayXR.CaptureAtlas` console / Blueprint wrappers,
  the `NextSequenceNumber`/`MakeOutputPath` naming, and the Win32 flash overlay —
  all repointed at `xrCaptureAtlasEXT`.

### Engine-plugin specifics (Unity + Unreal)

The engines differ from native test apps in three ways the migration must respect:

1. **They capture an engine-side atlas RT, not the OpenXR swapchain.** Unity's
   `CaptureOnDemand` re-renders the rig; Unreal's `ProcessRequest_RenderThread`
   reads the engine's composed `AtlasSrc` on the render thread. Both read
   **synchronously, this frame**. `xrCaptureAtlasEXT` instead captures what the
   engine *submitted* to the runtime — equivalent pixels (`PROJECTION_ONLY` ==
   the submitted projection atlas), but captured on the **next composed frame**
   via the blocking handler. For a manual keypress this is invisible; a future
   per-frame/recording use would need the async variant (see open questions).
2. **They must opt into the extension.** The DisplayXR Unity `OpenXRFeature` and
   the Unreal `IOpenXRExtensionPlugin` have to add `XR_EXT_atlas_capture` to
   their requested-extensions list at instance creation and resolve
   `xrCaptureAtlasEXT` through their existing `xrGetInstanceProcAddr` dispatch —
   the same path they already use for `XR_EXT_display_info` / window binding.
3. **Net effect is a simplification, not just a move.** Both lose their riskiest
   code (Unity's hidden-camera Kooima re-render; Unreal's render-thread surface
   readback) and gain the ability to capture the true post-compose atlas, which
   neither can do today.

**`displayxr-shell-pvt`**:
- No deletion required. The shell keeps `xrCaptureWorkspaceFrameEXT` (privileged
  whole-composite capture). Optionally pass the new `PROJECTION_ONLY` flag for a
  pre-HUD variant.

Net: ~5 readback implementations × 3 copies + 2 engine re-ports → **one** runtime
implementation already in the tree. The only per-app code that legitimately
remains is the input binding and the platform flash affordance.

## Migration sequencing

Append-only extension addition + a workspace flag bump → coupled release, same
recipe as prior extension rollouts (see the coupled-extension merge-order notes):

1. Land `XR_EXT_atlas_capture.h` + runtime wiring + workspace
   `PROJECTION_ONLY` flag (spec_version bump) in this repo. Bump
   `XR_EXT_spatial_workspace_SPEC_VERSION`.
2. Let `publish-extensions` auto-sync headers to `displayxr-extensions`.
3. Migrate consumers independently (no lock-step — old apps keep their forked
   helper until they adopt): test apps first (in-tree, proves both in-process and
   the metadata path), then demos, then Unity/Unreal, then optionally the shell's
   pre-HUD variant.
4. Delete each fork only after its consumer ships against the runtime that
   exports `xrCaptureAtlasEXT` (extension is queryable, so apps can feature-detect
   and fall back during the transition).

## Open questions

- **Result path ownership.** Should the runtime own filename numbering
  (`<stem>-<N>_<cols>x<rows>.png`) and return the final path, or keep that
  app-side? Owning it removes the last shared helper but means the runtime must
  learn the app's preferred stem.
- **Synchronous vs async.** `mcp_capture_blocking_handler` blocks up to 3 s. Fine
  for a manual `I` keypress; a future recording mode (see
  [3d-capture.md](3d-capture.md)) needs an async/streaming variant.
- **In-process metadata completeness.** The service path fills eye poses from DP
  state; confirm the in-process `mcp_capture` path can populate `eyeLeftM/eyeRightM`
  (or document them as zero for non-workspace sessions).
- **Relationship to [3d-capture.md](3d-capture.md).** That proposal is the
  *user-facing* L/R 3D-capture/recording feature (issues #43/#44). This doc is
  the *developer/debug* single-frame atlas API. They should share the runtime
  capture point; decide whether `xrCaptureAtlasEXT` is the MVP substrate for it.
- **Metal projection-only.** The metal compositor's `PROJECTION_ONLY` split is
  still stubbed (no command-buffer split). `POST_COMPOSE` works; document the
  gap or finish the split before claiming full coverage on macOS.
```
