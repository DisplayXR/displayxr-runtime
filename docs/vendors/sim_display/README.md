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
- **Interlaced** — 1-pixel-period column interlace: view 0 on even panel
  columns, view 1 on odd, both sampled at the same UV. The only
  phase-sensitive mode — see the next section.

Mode is selected at driver init time
(`SIM_DISPLAY_OUTPUT=2d|anaglyph|sbs|squeezed|quad|blend|interlaced`). See
`sim_display_processor.c` for the mode dispatch and `shaders/` for the
per-mode compositing kernels.

## What sim_display does NOT validate — weave geometry

With the one exception of **interlaced** (below), none of the output modes
above is column-interlaced, so **none of them is sensitive to the woven
texture's exact physical pixel size, or to where that texture lands on the
panel**. A fullscreen triangle renders correctly at any size and any offset, so
sim_display produces a plausible image whether or not the geometry is right.

A real lenticular weaver is sensitive to both. Its interlacing phase is a
function of the target's absolute physical-pixel origin, and any resample
between the woven texture and scanout destroys the pattern outright — a
half-pixel error is visible, and a desktop running at non-100% scale is fatal.

So a green sim_display run establishes plug-in discovery, the display-processor
path, session and swapchain creation, the frame loop, and that the compositor
hands the DP an atlas. It establishes **nothing** about geometric correctness.
Do not conclude from it that weaving will work.

### `SIM_DISPLAY_OUTPUT=interlaced` — making size and phase errors visible

`SIM_DISPLAY_OUTPUT=interlaced` is a phase-sensitive *proxy* for a lenticular
weave. The output pixel at panel column `X` shows view 0 when
`(X + phase) % 2 == 0` and view 1 otherwise, both sampled at the same
normalized UV — position-preserving like anaglyph, not a rearranging layout
like SBS. `phase` is the weave target's panel-relative X origin
(`canvas_offset_x`, as handed to `process_atlas`).

That gives it the same two failure modes as the real thing:

- **Any resample** — desktop scale != 100%, a mis-sized swapchain, a
  compositing display server scaling the surface — smears the alternating
  columns into flat grey or a moire beat. A correct 1:1 path stays crisp.
- **A wrong origin** flips which eye lands on the even columns, so the image
  goes pseudoscopic (inverted depth) rather than merely shifting.

`SIM_DISPLAY_INTERLACE_PERIOD=N` widens the stripes to `N` pixels so the
pattern — and the phase shift as the window moves — is visible by eye. The
default is 1, because 1 is what a lenticular actually needs and what makes the
mode sensitive.

It is still a proxy. There is **no lens model** here: no slant, no pitch, no
per-view crosstalk, no per-vendor calibration. Interlaced can show you that the
geometry is wrong; it can never show you that a real vendor weave is right.
That still needs vendor hardware, or a measurement that is itself
phase-sensitive.

Backend support: implemented on the **Vulkan** and **OpenGL** display
processors. The **D3D11**, **D3D12** and **Metal** processors fall back to
anaglyph and log a one-shot warning — they share a shader-constant-buffer
layout that the phase would have to be threaded through, which has not been
done yet.

`SIM_DISPLAY_STRICT_PANEL=1` narrows that gap. It makes the Vulkan DP report,
whenever the geometry changes, the weave target it received against the panel
dimensions the driver declared (`SIM_DISPLAY_PIXEL_W`/`SIM_DISPLAY_PIXEL_H`),
plus the canvas rect the compositor asked for:

```
sim_display STRICT PANEL (#817): weave target 2880x1800, panel 2880x1800 (0.300x0.190 m)
  — target is EXACTLY panel-sized (a real weaver gets 1:1 pixels)
sim_display STRICT PANEL (#817): canvas offset (0,0) size 0x0 (panel origin);
  atlas view 1440x900 grid 2x1. ...
```

Because `SIM_DISPLAY_PIXEL_W/H` propagates all the way through (app window →
swapchain → atlas → weave target), this lets a target panel's exact geometry be
staged and verified before that hardware exists.

Two limits are worth stating plainly. The audit compares the target against the
**declared** panel, which is not the same as the **physical** panel — under a
scaled desktop the two diverge, and only the runtime's own desktop-rect resolver
(`display_desktop_rect_is_panel`, surfaced to apps as `isPanelConfirmed`) sees
it. And sim_display implements no `set_present_origin` slot at all, so in every
mode *except* interlaced a phase error is invisible in what it draws — the
audit reports the offset, and the interlaced output is what actually reacts to
it. The option is diagnostic only; it never changes what is rendered.

## Eye-tracking mode

sim_display reports **MANAGED** mode and feeds a fixed eye position (no actual tracking). The MANAGED/MANUAL contract is in [`docs/specs/vendor/eye-tracking-modes.md`](../../specs/vendor/eye-tracking-modes.md).

## When to use

- Local development on machines without a 3D display
- CI builds and headless tests
- Reference implementation when writing a new vendor — `sim_display_processor.c` is the smallest complete DP implementation in the tree.

For the generic vendor contract, see [`docs/guides/vendor-plugin-onboarding.md`](../../guides/vendor-plugin-onboarding.md) and [`docs/specs/vendor/display-processor-interface.md`](../../specs/vendor/display-processor-interface.md).
