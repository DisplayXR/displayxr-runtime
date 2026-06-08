---
status: Accepted
date: 2026-06-08
issues: [396, 425]
---
# ADR-023: Unified Atlas Capture (XR_EXT_atlas_capture)

## Context

"Snapshot the multi-view atlas to a PNG" (usually the `I` key; `Ctrl+Shift+C`
in the shell) was reimplemented in every consumer — test apps, both demos, the
Unity and Unreal plugins — each doing its own GPU→CPU readback against its own
**pre-submit swapchain**. Roughly five per-API readback paths × three C++
source copies + two engine-native re-ports, forked again by every new vendor
demo.

Two structural problems:

1. **Duplication.** Every consumer carries (and forks) the readback.
2. **Wrong capture point.** An app can only read its own pre-submit swapchain —
   projection content only. It physically cannot see runtime-composed
   window-space / quad layers, the cursor, or workspace chrome. There is no way
   for an app to get "what the display processor actually saw."

Meanwhile the runtime already owns everything needed to do this centrally:

- **`u_capture_intent`** (every in-process compositor) with the two-mode
  distinction `POST_COMPOSE` (DP-bound atlas) vs `PROJECTION_ONLY`, driven by
  dev trigger files + the MCP `capture_frame` tool, with per-API readback in
  each compositor's `*_capture_atlas_to_png`.
- **`xrCaptureWorkspaceFrameEXT`** (`XR_EXT_spatial_workspace`) — a real OpenXR
  function, but privileged (workspace controller only), IPC + D3D11 only, and
  post-compose only.

The gap was purely plumbing: a non-privileged, all-session, all-API,
mode-flagged entry point on top of the existing readback core.

## Decision

Add a vendor-neutral extension **`XR_EXT_atlas_capture`** with one function
`xrCaptureAtlasEXT(session, info, result)`. It captures the atlas the runtime
composes **for the calling session** at a caller-selected stage
(`PROJECTION_ONLY` / `POST_COMPOSE`) and returns the same metadata block
(atlas/eye dims, tile layout, eye poses) the workspace capture returns.

- **Any session** may call it (handle / texture / hosted / IPC). In-process
  sessions drive the compositor's `u_capture_intent` via the existing
  `mcp_capture` hand-off; IPC sessions route over the workspace-capture IPC
  bridge.
- **All graphics APIs** are covered — the runtime does the readback with the
  compositor's own `*_capture_atlas_to_png`; the app never touches a staging
  texture.
- **`xrCaptureWorkspaceFrameEXT` stays** as the privileged *cross-client*
  capture (the whole workspace composite — inherently a workspace concern),
  reimplemented on the shared readback core and gaining the `PROJECTION_ONLY`
  flag. Apps move to `xrCaptureAtlasEXT`; the shell keeps the workspace call.

A **new** extension rather than ungating the workspace one: a universal
all-apps capture must not live behind a "workspace controller" privilege or
carry `Workspace` in its name (the workspace extension is a privileged,
customer-facing surface, not the home for a universal app feature). The
unification is at the shared runtime readback layer, which both extensions use.

### Filename + alpha contract (#425)

- The runtime appends `_atlas_<viewCount>_<cols>x<rows>.png` to the caller's
  `pathPrefix` (e.g. a 2-view 2×1 capture → `<prefix>_atlas_2_2x1.png`) so
  consumers don't re-derive atlas geometry. Built at the EXT-contract layer
  (`oxr_capture.c` in-process, `comp_d3d11_service.cpp` IPC) — **not** inside the
  per-API `*_capture_atlas_to_png`, because the MCP `capture_frame` tool and the
  dev trigger files report a verbatim full path.
- Every encoder forces `A=255` before PNG write (`u_image_force_opaque_rgba8`):
  display-output alpha is undefined (the DP/weaver ignores it); left verbatim it
  reads back as 0 → fully transparent → renders black.

## Consequences

- One runtime readback implementation replaces the per-API paths, the source
  copies, and the engine re-ports. The only legitimately per-app code left is
  the input binding and the platform flash affordance.
- Apps gain the ability to capture the true post-compose atlas (chrome, cursor,
  quads) — impossible before.
- Append-only extension + a workspace-flag bump → coupled release; consumers
  migrate independently (feature-detectable, falling back to their fork during
  the transition).
- Engine plugins request the extension at instance creation and resolve the PFN
  through their existing `xrGetInstanceProcAddr` dispatch; they trade their
  riskiest code (Unity's hidden-camera Kooima re-render, Unreal's render-thread
  surface readback) for the call.
- Open follow-ups: an async/streaming variant for a future recording mode (the
  current handler blocks up to 3 s); in-process eye-pose metadata completeness;
  the metal `PROJECTION_ONLY` split is still stubbed (`POST_COMPOSE` works).

## References

- Header `src/external/openxr_includes/openxr/XR_EXT_atlas_capture.h`; impl
  `src/xrt/state_trackers/oxr/oxr_capture.c`.
- `XR_EXT_spatial_workspace` (`xrCaptureWorkspaceFrameEXT` + `PROJECTION_ONLY`).
- Shipped in runtime v1.10.0. Epic #396 (W6); filename/alpha contract #425.
- Related: `docs/roadmap/3d-capture.md` (the user-facing L/R capture feature).
