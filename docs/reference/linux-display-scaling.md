# Linux display scaling and windowed weaving

**Short version.** On GNOME/XWayland, **one scaled display anywhere on the desktop
limits where every X11 window can go, on every display, the 3D panel included.** The
reason is that XWayland scales the whole X screen by one global factor, and that factor
comes from the most-scaled output. At a factor of 2, a window origin can only land on
even device pixels. The weave phase depends on the window's exact physical-pixel
position, so a drag-time phase snap that asks for an odd pixel is silently rounded, and
the 3D stutters while the window moves.

**Supported configuration today: every output at 100%.** With that, the window reaches
every pixel and drag snapping is exact. This was measured on the DS1 and confirmed by eye
(2026-09-20). Other configurations are detected and handled as described below. Nothing
here claims they are as good.

Windows does not have this problem. A per-monitor-DPI-aware process gets physical-pixel
window coordinates at any scale (see [dpi-awareness.md](dpi-awareness.md)), and there is
no equivalent on Linux. See [Can it be made scale-proof?](#can-it-be-made-scale-proof).

## The mechanism

Mutter's `xwayland-native-scaling` (on by default alongside
`scale-monitor-framebuffer` on GNOME 50) gives X11 a "protocol" coordinate space. That
space is Mutter's logical stage multiplied by **N = the ceiling of the largest monitor
scale** ([mutter!3567](https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/3567)).
Window positions are stored in logical units, so an X11 origin `x` becomes `x / N`, gets
rounded, and comes back as a multiple of `N`.

Here are three configurations measured on one box: laptop eDP-1 2880x1800 next to an
Acer SpatialLabs DS1 3840x2160.

| laptop | DS1 | X11 sees DS1 | X11 sees laptop | X root | N | result |
|---|---|---|---|---|---|---|
| 166% | 200% | 3840x2160 (= native) | 3456x2160 | 7296x2160 | 2 | **every size check passes; 0% of odd targets reached; stutters** |
| 166% | 100% | 7680x4320 | 3456x2160 | 11136x4320 | 2 | the panel is not found by size; phase feed and snap refused |
| 100% | 100% | 3840x2160 | 2880x1800 | 6720x2160 | 1 | ~50% odd reached, 92% of snaps landed exactly; correct by eye |

The first row is the dangerous one. The panel's own numbers are perfect, so any check
that looks only at the panel cannot find it. Only the laptop's mismatch gives it away,
which is why the solver checks every output. Either display alone at >100% would force
`N = 2`: the panel's 200% by itself, or the laptop's 166% next to a panel at 100%
(row 2).

## What the runtime does

| Where | What |
|---|---|
| `util/u_x11_scale.h` (header-only, host-tested) | The solver. For every output it compares the X11 size with the DRM mode. `N` is the smallest integer where each derived scale `native·N/X11` is ≥ 1 and `max(ceil(scale)) == N`. It also provides the reachable-lattice enumeration and the landing probe. |
| `os/os_display_scale_linux.c` | Joins RandR monitors (`os_display_desktop_enumerate`) to `/sys/class/drm/card*-*/modes` (no libdrm, no device open) and runs the solver. |
| `comp_vk_native_compositor_snap_window_rect` (X11 drags) | **Units gate:** if the panel's X11 rect is not its native size, X11 px are not panel px, so the snap is refused. This matches the existing present-origin refusal. **Quantum > 1:** the snap is searched on the reachable lattice `origin + N·Z²`, offering candidates to the DP and keeping the first one it snaps to a reachable point. The DP still owns all lens math (ADR-019). |
| displayxr-common `common/linux/dxr_linux_window.cpp` | **Landing check.** Before each drag move, it reads back where the previous move actually landed. If the moves persistently miss (≥6 moves, at least half diverging, inferred stride > 1), it logs one `drag: placement NOT honoured` WARN that names the stride and the likely cause. This catches causes the geometry cannot see. |
| `displayxr-cli info` → *X11 coordinate space* | The verdict, the quantum, each output's X11 size, DRM mode and derived scale, the X root compared with native width, and a one-line *drag snap* capability: yes / PARTIAL / NO / unknown. |
| `displayxr-cli selftest` → `x11_placement` | **Informational, never fails.** A scaled output is a user setting, not a broken install. The detail line carries `WARNING:` when quantised. |

**Weaving is never stopped over this.** A quantised placement does not make a static or
fullscreen window wrong: the window sits at *some* origin, and the runtime feeds the
weaver that true origin, so the weave is correctly phased. What a quantum breaks is
*honouring an arbitrary snap target during a drag*. So that is the only thing this code
changes. #1595's refuse-into-2D answers a different fault: a buffer that reaches glass
resampled can never be correct, at any position.

`DXR_X11_PLACEMENT_QUANTUM=<n>` forces the quantum. It is meant for testing, and for the
blind spot below.

### The solver's blind spot

If every output runs at the same integer scale (for example all at 200%), each X11 size
equals its native mode, and `N = 1` is also consistent. Geometry cannot tell that apart
from all outputs at 100%. The solver reports "device pixels", `info` says so, and the
landing check is what catches it. Querying `wl_output`/`xdg_output` directly would close
the gap. That is not done yet.

## Can it be made scale-proof?

Here is each option, and whether it removes the dependency on the user's scale settings.

1. **An X11 or XWayland option that gives device-pixel placement.** None found. The
   quantum comes from Mutter storing window positions in logical units.
   `xwayland-native-scaling` is what gives the X screen its uniform ×N space. Turning it
   off would run X at logical scale 1, which makes the panel 1:1 only when the *panel* is
   at 100%, and upscales (blurs) X11 apps on the scaled laptop. That is plausible but
   **not measured**, so it is not recommended yet.
2. **Native Wayland.** This is not better for dragging. A toplevel cannot position itself
   (xdg-shell has no positioning), cannot learn its own position without the
   window-geometry extension, and cannot own its drag. Positions are logical, so the
   device quantum on the panel equals the panel's own scale. `wp_viewporter` and
   `wp_fractional_scale_v1` fix buffer *size* (1:1 pixels), not *position*. The one real
   difference is that Wayland is per-output: with the panel at 100%, a scaled laptop does
   not affect it.
3. **A placement-stride-aware snap.** This is what is implemented, done on the runtime
   side with the DP as an oracle, so there is **no ABI change and no vendor change**. It
   keeps the 3D correctly phased under a quantum. The cost is coarser drag steps. The
   radius-2 vendor snap has no reachable same-phase point near many targets, so the
   runtime searches up to three lattice rings (≤ 3·N px) and falls back to the nearest
   reachable pixel with one WARN. It fixes configuration 1 (the dangerous one). It cannot
   fix configuration 2, where the units are wrong and not just the stride. That needs an
   X11→device conversion by the ratio `X11/native` of the panel, a contained follow-up.
   **Needs a hardware run** to confirm the reachable density is adequate.
4. **Fixed-origin surface: stop depending on window position.** This is the only
   *complete* answer. The app presents into a panel-aligned, panel-sized surface whose
   origin never moves, and the runtime composes the app's content at a pixel offset
   inside it. The weave origin is constant, so it is always phased and immune to both the
   quantum and origin-feed latency, while the visible "window" moves by single pixels.
   **It is the DisplayXR shell's compose model**, which does not exist on Linux. Its cost:
   an ARGB, input-shaped (XShape / `wl_surface.set_input_region`) full-panel window per
   app or one shell surface; a content offset in the vk_native compose; input
   re-targeting; stacking and focus semantics against real desktop windows; and
   verification that the vendor DP weaves a partially transparent surround acceptably.
   **Estimate: 1.5–3 weeks for a single-app prototype on X11; the product form is a Linux
   shell.** It is deliberately not attempted here.

Recommendation: ship (3) plus detection and honest reporting now; document 100% on every
output as the supported configuration; hardware-validate (3) under 166%/200%; do the
units conversion for configuration 2 next; and treat (4) as the path to real
scale-independence, sized and scheduled with the Linux shell.
