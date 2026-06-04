# <APP> — DisplayXR app

A DisplayXR OpenXR app (scaffolded from a `cube_*` reference via `/new-displayxr-app`). It couples
to the runtime **only** through the OpenXR API + the `XR_EXT_*` extensions — never include
runtime-internal source.

## Before you change rendering / session code, read the rules

The authoring invariants live in the runtime repo at
`docs/guides/displayxr-app-rules.md` (public:
https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/guides/displayxr-app-rules.md).
They are binding. The traps agents hit most:

- **INV-3.1** — `xrLocateViews` fills an **8-wide** (`XRT_MAX_VIEWS`) buffer; render/submit the
  **active mode's `viewCount`** (1/2/4), never a hardcoded 2.
- **INV-4.3** — per-tile size = **window/canvas × `recommendedViewScaleX/Y`**, never display size.
- **INV-4.6** — request an **sRGB swapchain** and store a correctly-encoded image (render linear
  with GPU sRGB-write, *or* write display-referred bytes — not both). Data textures stay linear.
- **INV-2.4** — the runtime owns the active mode; you *request* via
  `xrRequestDisplayRenderingModeEXT` and update local state only on the change **event**.
- **INV-7.1/7.2** — capture via `xrCaptureAtlasEXT` (prefix with **no** extension); never
  reintroduce an app-side `CaptureAtlasRegion*` readback.
- **INV-9.1/9.2** — ship `<exe>.displayxr.json` (schema 1; `name`; `type`) + `icon.png` (512×512) +
  `icon_sbs.png` (1024×512, `sbs-lr`) or the app won't appear in the workspace launcher.
- **F-1…F-6** — the baseline OpenXR mechanics (session/frame loop, swapchain
  acquire/wait/release, graphics-requirements + adapter matching, LOCAL space, extension
  enablement). Reuse `test_apps/common/` rather than re-implementing.

## Lint before you call it done

```
python3 scripts/check_displayxr_app.py <this-app-dir>
```
Fix every ERROR; a clean run is the bar. (`--list-rules` prints the catalog.)

## TODO for this scaffold
- Replace the placeholder `displayxr/icon.png` + `icon_sbs.png` with real 2D/3D art (same sizes).
- Fill in `name` / `description` in `displayxr/<exe>.displayxr.json`.
- Replace the placeholder cube scene with the app's actual content (in the render callback).
