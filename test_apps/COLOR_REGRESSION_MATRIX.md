<!--
Copyright 2026, Leia Inc.
SPDX-License-Identifier: BSL-1.0
-->
# Color-management regression matrix (ADR-021)

Manual verification for the encoding-state contract and Model A/B. Closes the
ADR-021 verification gap ("no test app renders true-linear / honest-sRGB"). The
multi-layer-blend and workspace cases need the live display + shell, so this is a
manual procedure, not an automated test.

## Driving the encoding axis

The cube test apps select their color swapchain via `DXR_SWAPCHAIN_ENCODING`
(handled in `common/xr_session_common.cpp::SelectColorSwapchainFormat`):

| Value | Swapchain | Bytes reaching the runtime | Exercises |
|---|---|---|---|
| `srgb` | an advertised `*_SRGB` format | GPU auto-encodes on the per-frame RTV write → **honest encoded**; runtime sets `atlas_holds_srgb_bytes=true` | the **Model B decode leg** when ≥2 such clients blend |
| `unorm` | a plain UNORM format | app writes raw → **encoded-into-UNORM** (today's default) / **true-linear-into-UNORM** (content = whatever the renderer wrote) | Model A passthrough |
| unset | runtime-preferred (`formats[0]`) | unchanged | default behavior |

Set it process-level (the runtime DLL has its own static-CRT env block; use a real
env var, not a run-script line):
```cmd
set DXR_SWAPCHAIN_ENCODING=srgb && test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

## Matrix

`{sRGB swapchain, UNORM-encoded, true-linear} × {single-layer opaque, multi-layer blend} × {in-process, IPC, workspace}`

- **Encoding** → `DXR_SWAPCHAIN_ENCODING` (`srgb` / `unorm`). "true-linear" = `unorm` with the renderer writing linear radiance.
- **Single-layer** → one app. **Multi-layer blend** → two overlapping windows under the shell (or a transparent-bg app: `DISPLAYXR_TRANSPARENT_BG=1`).
- **Transport** → in-process (default), IPC (`set XRT_FORCE_MODE=ipc` + running `displayxr-service.exe`), workspace (launch under `displayxr-shell.exe`).

Model B engages **only** in the workspace/service multi-layer column with honest-`srgb` clients against a `LINEAR`/`EITHER` DP; every other cell is Model A passthrough (B == A when nothing blends), so the in-process fast path is identical across encodings.

## In-repo DP test double

`sim_display`'s **D3D11** variant declares `XRT_DP_COLOR_EITHER` and applies the
standard sRGB OETF on output when the runtime declares a `LINEAR` atlas
(`out_encode()` in `sim_display_processor_d3d11.cpp`). It is the hardware-free
exerciser of the Model-B encode-at-handoff direction — register the freshly built
`DisplayXR-SimDisplay.dll` and run two `DXR_SWAPCHAIN_ENCODING=srgb` cubes under
the shell. (The VK/GL/Metal sim_display variants are in-process-only and stay
`ENCODED` passthrough.)

## Capture verification

The D3D11 service compositor captures the combined atlas **post-compose, pre-DP**:
```bash
rm -f "$TEMP/workspace_screenshot_atlas.png"
touch "$TEMP/workspace_screenshot_trigger"   # wait ~3s, then read the PNG
```
- **No double-encode (Model A):** cubes are UNORM-encoded; the captured atlas bytes must equal the app's output (no ~2.2× darkening).
- **Model B:** the captured atlas is intentionally **linear** (numerically darker) — the encode happens *after* capture, in the DP — so a dark pre-DP atlas under B is expected, not a regression. Confirm the on-screen (post-DP) result is correct by eyeballing the live display (screenshots during eye-tracking warmup miss UI).
