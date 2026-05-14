# Leia SR Integration

Leia is the first 3D-display vendor integrated into DisplayXR. This directory documents the Leia-specific implementation; the generic vendor contract lives in [`docs/specs/vendor/`](../../specs/vendor/) and [`docs/guides/vendor-integration.md`](../../guides/vendor-integration.md).

## Source layout

`src/xrt/drivers/leia/`:

| Group | Files |
|---|---|
| Driver entry / device | `leia_device.c`, `leia_interface.h`, `leia_types.h`, `leia_edid_probe.c`, `leia_sr_probe.cpp` |
| SR SDK bridge | `leia_cnsdk.{cpp,h}` |
| Display processor (per API) | `leia_display_processor.{cpp,h}` (base), `leia_display_processor_d3d11.{cpp,h}`, `leia_display_processor_d3d12.{cpp,h}`, `leia_display_processor_gl.{cpp,h}` |
| Weaver (per API) | `leia_sr.{cpp,h}` (base + eye tracking), `leia_sr_d3d11.{cpp,h}`, `leia_sr_d3d12.{cpp,h}`, `leia_sr_gl.{cpp,h}` |
| Background capture (transparency) | `leia_bg_capture_win.{cpp,h}` |
| Shaders | `shaders/` |

## Docs in this directory

- **[Weaver internals](weaver.md)** — DX11 / DX12 / OpenGL / Vulkan weaver creation, inputs, weave() flow, DPI handling, phase math.
- **[Transparency model](transparency.md)** — current primary path: WGC compose-under-bg on D3D11 / D3D12 / Vulkan. Replaces the older chroma-key approach for those APIs.
- **[Chroma-key overlay (legacy / OpenGL fallback)](chroma-key-overlay.md)** — fallback path; still the only transparency path on the Leia OpenGL DP.

## Build flags

- `XRT_HAVE_LEIA_SR` — auto-enabled when `LEIASR_SDKROOT` env var is set and `find_package(simulatedreality CONFIG)` succeeds.
- `XRT_HAVE_LEIA_SR_D3D11`, `XRT_HAVE_LEIA_SR_D3D12`, `XRT_HAVE_LEIA_SR_GL`, `XRT_HAVE_LEIA_SR_VULKAN` — per-API weaver gates.

See the root `CLAUDE.md` "LeiaSR SDK Integration" section for build details.

## Eye-tracking mode

Leia ships in **MANAGED** mode (the runtime polls SR SDK's `LookaroundFilter` on Leia's behalf). The MANAGED/MANUAL contract is described in [`docs/specs/vendor/eye-tracking-modes.md`](../../specs/vendor/eye-tracking-modes.md).
