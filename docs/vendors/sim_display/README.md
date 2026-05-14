# sim_display Integration

`sim_display` is the reference simulation vendor that ships with DisplayXR. It renders to a normal 2D window using side-by-side / anaglyph / blend modes, so contributors can develop and test the runtime without 3D-display hardware. It also serves as the canonical example of a minimal display processor implementation.

## Source layout

`src/xrt/drivers/sim_display/`:

| File | Purpose |
|---|---|
| `sim_display_device.c` | Device enumeration + init |
| `sim_display_interface.h` | Public driver interface |
| `sim_display_macos.m` | macOS-specific window setup |
| `sim_display_processor.c` | Base display processor (mode logic) |
| `sim_display_processor_d3d11.cpp` | D3D11 DP variant |
| `sim_display_processor_d3d12.cpp` | D3D12 DP variant |
| `sim_display_processor_gl.c` | OpenGL DP variant |
| `sim_display_processor_metal.m` | Metal DP variant |
| `shaders/` | Side-by-side / anaglyph compositing shaders |

## Display modes

sim_display supports several output modes for visualizing stereo content on a 2D screen:

- **Side-by-side (SBS)** — left view in left half, right view in right half. Default.
- **Anaglyph (red/cyan)** — for use with red/cyan glasses.
- **Blend** — interleaved or alpha-blended views for quick visual sanity checks.

Mode is selected at driver init time. See `sim_display_processor.c` for the mode dispatch and `shaders/` for the per-mode compositing kernels.

## Eye-tracking mode

sim_display reports **MANAGED** mode and feeds a fixed eye position (no actual tracking). The MANAGED/MANUAL contract is in [`docs/specs/vendor/eye-tracking-modes.md`](../../specs/vendor/eye-tracking-modes.md).

## When to use

- Local development on machines without a 3D display
- CI builds and headless tests
- Reference implementation when writing a new vendor — `sim_display_processor.c` is the smallest complete DP implementation in the tree.

For the generic vendor contract, see [`docs/guides/vendor-integration.md`](../../guides/vendor-integration.md) and [`docs/specs/vendor/display-processor-interface.md`](../../specs/vendor/display-processor-interface.md).
