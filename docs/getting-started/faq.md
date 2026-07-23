# FAQ

Conceptual questions about what DisplayXR is and how to build for it. For
symptom → cause → fix when something breaks, see **[Troubleshooting](troubleshooting.md)**.

---

### What is DisplayXR?

An open-source [OpenXR](https://www.khronos.org/openxr/) runtime for **glasses-free 3D
displays** — autostereoscopic monitors and laptops that deliver head-tracked stereoscopic
3D without any worn hardware. It implements the Khronos OpenXR API, so apps target the
industry standard instead of a per-vendor SDK. See the
[Overview](overview.md) for the full picture.

### Why not just use each vendor's SDK?

Because apps built against one vendor's SDK don't run on another vendor's hardware.
DisplayXR is a single, **vendor-neutral** runtime: write to OpenXR once and run on any
supported 3D display. Vendors plug in underneath (see *How do display vendors integrate?*).

### Do I need a 3D display to develop?

**No.** The runtime ships a hardware-free **sim-display** plug-in that presents a virtual
3D display with valid dimensions and view geometry. You can build, run the self-test, and
develop most of an app without any special hardware. You only need a real 3D display to see
the final woven 3D image and to exercise real eye tracking.

### Which displays / hardware are supported?

DisplayXR is display-agnostic — any 3D-display vendor integrates via a plug-in DLL. **Leia
SR** is the first shipping integration. Other vendors integrate by shipping their own
plug-in (see *How do display vendors integrate?*).

### Which operating systems and graphics APIs?

- **OS:** Windows and macOS ship today; Linux (Vulkan-only, X11/XCB) is a hardware-validated
  preview; Android is supported.
- **Graphics APIs:** each API gets a **native compositor** — D3D11, D3D12, Vulkan, Metal,
  and OpenGL — with no interop layer or Vulkan intermediary. On Windows all five are
  available; Linux is Vulkan-only; macOS uses Metal/Vulkan(MoltenVK)/GL.

### Is this "real" OpenXR? Can an existing OpenXR app just run?

Yes, DisplayXR implements the Khronos OpenXR API, and apps use the standard OpenXR entry
points. A few DisplayXR-specific things to know:

- **Windowing** uses small custom extensions (`XR_DXR_win32_window_binding`,
  `XR_DXR_cocoa_window_binding`, `XR_DXR_xlib_window_binding`) so your app can hand its
  window to the runtime.
- **3D rendering is multi-view.** A 3D display shows more than two perspectives, so a
  fully 3D app renders **N views** (tiles), not a fixed left/right pair. An unmodified
  two-view app can still run in modes with two or fewer views, but it can't drive richer
  multi-view modes. See [Kooima Projection](../architecture/kooima-projection.md) and the
  multiview specs.

### Why do the docs say "views" / "tiles" and never "stereo"?

Because these displays aren't limited to two eyes. The runtime thinks in **views** (per-eye
perspectives), **tiles** (their layout in a swapchain), and an **atlas** (the packed set of
tiles). "Stereo"/"SBS" implies exactly two, which is a special case — the general model is
N views.

### I use Unity / Unreal — do I write OpenXR calls myself?

No. Engine integrations handle it: the **DisplayXR Unity** plug-in and the Unreal HMD plug-in
drive OpenXR under the hood. Unity submits its two views through the native compositor;
Unreal renders adaptive N-view. You build your scene as usual in the engine.

### What's the difference between the runtime, the shell, and a vendor plug-in?

- **Runtime** — this repo: the OpenXR runtime, compositors, and IPC. Vendor-neutral.
- **Vendor plug-in** — a separate DLL (shipped from the vendor's own repo) that drives a
  specific display and does the weaving. Discovered and loaded at `xrCreateInstance`.
- **Shell / workspace controller** — an optional spatial desktop that hosts multiple apps
  in 3D. It ships separately and registers with the runtime; the runtime itself owns no
  particular workspace app.

### How do display vendors integrate?

A vendor ships a **plug-in DLL** implementing the runtime's display-processor interface,
discovered at startup (registry on Windows, JSON manifest on POSIX). The runtime binary
contains no vendor-specific code. Start with the
[Vendor Plug-in Onboarding](../guides/vendor-plugin-onboarding.md) guide.

### Is it open source? What license?

Yes. The runtime is licensed under the **Boost Software License 1.0** (BSL-1.0), a permissive
open-source license. External contributors can PR directly against
[the runtime repo](https://github.com/DisplayXR/displayxr-runtime).

### Is DisplayXR related to Monado?

It began as a fork of [Monado](https://monado.freedesktop.org/) and shares the `xrt`
interface heritage, but it's purpose-built for 3D displays: native per-API compositors, a
vendor display-processor plug-in model, and 3D-display-specific view/tiling and projection.

### Does it do eye tracking?

Yes, on hardware that provides it — through the vendor plug-in, which reports supported
eye-tracking modes via `XR_DXR_display_info`. Without hardware, sim-display can simulate
tracking for testing.

### Where do I start building?

- [Overview](overview.md) — what DisplayXR is and how it's built
- [Building](building.md) — Windows / macOS / Linux build instructions
- [App Classes](app-classes.md) — handle / texture / hosted / IPC
- [Your First Handle App](first-handle-app.md) — tutorial
- [Ship a Manifest](ship-a-manifest.md) — make your app discoverable
- [Troubleshooting](troubleshooting.md) — when something goes wrong
