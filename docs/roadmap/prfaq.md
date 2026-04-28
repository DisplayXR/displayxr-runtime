# DisplayXR PRFAQ — An Open Platform for 3D Displays

> **Status**: draft, internal review. Source for upcoming website copy. Last updated 2026-04-28.

---

## Press release

### DisplayXR Becomes a True Platform: Open Runtime, Standardized Extensions, Swappable Workspace Controllers

**A lightweight OpenXR runtime, a documented extension surface, and a reference workspace controller — so anyone building on a 3D display can ship without forking the stack.**

3D displays have spent a decade trapped in vendor silos. Every panel ships with its own runtime, its own compositor, its own windowing model — and the application developer pays for it. Building a CAD viewer or a medical-imaging app for a 3D screen has meant committing to one vendor's stack, learning their proprietary APIs, and rewriting when the next display generation lands.

DisplayXR breaks that pattern. The DisplayXR runtime is a lightweight OpenXR implementation purpose-built for 3D displays — installable on its own, useful on its own, vendor-agnostic at every layer. Application developers write standard OpenXR. Display vendors (Leia, BOE, and others making the lightfield / autostereo panels) implement a documented integration interface. OEMs (Lenovo, Samsung, ZTE, and others integrating displays into finished laptops, monitors, tablets, phones, and TVs) ship the runtime as part of their product. And on top of the runtime, an optional **workspace controller** layer adds spatial-desktop features — windowing, multi-app composition, launcher UX — through open extensions any party can implement.

A reference workspace controller, **DisplayXR Shell**, ships alongside the runtime from the DisplayXR project as a proprietary, polished example of what's possible on the platform. It is also entirely optional. Install the DisplayXR runtime by itself and you get a standards-compliant OpenXR + WebXR bridge for your 3D display — no shell, no spatial desktop. Install a different controller (third-party, vertical integrator, your own) and the runtime treats it as a first-class citizen with the same authority as the reference shell.

"We brought our spatial-3D laptop SKU to market in weeks, not quarters, because we didn't have to build the OpenXR layer or the windowing system from scratch," said a hypothetical OEM partner who has not yet given us a real quote. "We bundled the reference shell, dropped a sidecar manifest to brand the tray for our product, and shipped. The DisplayXR runtime authenticated and composited it without modification."

The platform is built on three commitments:

- **Open at the runtime layer.** OpenXR + WebXR. Standard extension headers for display-specific capabilities (`XR_EXT_display_info`, `XR_EXT_win32_window_binding`, `XR_EXT_cocoa_window_binding`). Display-vendor integration through a documented display-processor vtable.
- **Open at the workspace layer.** `XR_EXT_spatial_workspace` and `XR_EXT_app_launcher` define how any controller process drives multi-app composition, hit-test, capture, and tile launching. The reference shell uses these surfaces; so does anyone else.
- **Swappable, not coupled.** Workspace activation is gated by orchestrator-PID match — the runtime trusts the binary it was configured to spawn, not a brand string. OEMs, vertical integrators, and developers can replace the controller without runtime modifications.

DisplayXR runs on Windows today, with macOS and Android in active development. The runtime ships under the Boost Software License. Extension headers are mirrored to `github.com/DisplayXR/displayxr-extensions` and version-tagged for stability. The reference shell is distributed separately from the runtime; a bare runtime install is the supported configuration for OEM and third-party deployments.

Get started at `github.com/DisplayXR/displayxr-runtime`.

---

## FAQ

### Glossary (terms used below)

- **Display vendor** — companies that manufacture the 3D display panel and write the display-processor (DP) integration for DisplayXR. Examples: Leia, BOE.
- **OEM** — companies that procure 3D displays from a display vendor and integrate them into a finished consumer product (laptop, monitor, tablet, phone, TV). Examples: Lenovo, Samsung, ZTE. OEMs are *customers* of the DisplayXR runtime and (optionally) the reference shell.
- **Vertical integrator** — companies building a focused single- or limited-purpose experience on a 3D display: CAD viewers, medical imaging, automotive HMI, retail kiosks. They consume the runtime and may write their own workspace controller.
- **Workspace controller** — a process that adds spatial-desktop features (windowing, multi-app composition, launcher) on top of the bare runtime. The reference DisplayXR Shell is one such controller; third parties can build their own.

### For application developers

**Q: I'm writing an OpenXR app. What changes for me?**
Nothing. Write standard OpenXR. The DisplayXR runtime registers as an OpenXR runtime via the standard loader — set `XR_RUNTIME_JSON` and your app finds it. Native compositors for D3D11, D3D12, Metal, Vulkan, and OpenGL mean your graphics API works without interop layers.

**Q: How do I make my app aware of the 3D display's capabilities?**
Opt into `XR_EXT_display_info` for display dimensions, eye-tracking modes, and the data needed for asymmetric (Kooima) projection. The header is published at `github.com/DisplayXR/displayxr-extensions` and frozen at v12. For window binding (passing your HWND or NSView to the runtime so it can composit into your window), use `XR_EXT_win32_window_binding` or `XR_EXT_cocoa_window_binding`.

**Q: Does my app need to know whether a workspace controller is installed?**
No. The runtime serves apps the same way whether the user is running the bare runtime, the DisplayXR Shell, or a third-party controller. Apps in a workspace get composited as one of N tiles; apps without a workspace render full-screen. From the app's perspective, OpenXR's session-state machine drives both cases identically.

**Q: I'm using WebXR in Chrome. How does that work?**
DisplayXR ships a WebXR bridge that connects Chrome's native WebXR implementation to the runtime. No extension to install in Chrome — the bridge runs as a separate process, started on-demand by the service when a WebXR app requests it. Standard WebXR session APIs work on a 3D display the same way they work on a headset.

### For 3D display vendors

**Q: I make a 3D display panel. How do I integrate with DisplayXR?**
Implement the display-processor vtable for your weaving / depth-mapping / eye-tracking integration. The vtable has five API variants (D3D11, D3D12, Metal, Vulkan, OpenGL) so your weaver runs natively in whatever graphics API the application is using. See `docs/guides/vendor-integration.md` for the full contract. Leia SR is the first integration; it's a reference, not a precedent — there is no privileged path.

**Q: Once my display is integrated, who picks DisplayXR up?**
Two distinct downstream audiences: OEMs who ship finished products built around your display panel, and vertical integrators who build a focused experience on it. Both groups consume the runtime via standard distribution channels (your driver bundles it, an OEM bundles it, or end users install it directly). Your DP integration enables both paths simultaneously.

**Q: Do I need to ship a workspace controller alongside my DP?**
No. The reference DisplayXR Shell works with any DP that conforms to the vtable. Most display vendors ship the DP plus the runtime and let OEMs decide whether to bundle the reference shell, a custom controller, or nothing. Display vendors who want their own branded controller can build one — see the OEM and vertical-integrator sections below for the controller story.

### For OEMs (Lenovo, Samsung, ZTE, integrators of finished products)

**Q: I'm shipping a 3D laptop / monitor / tablet. What do I bundle?**
Three reasonable configurations:

- **Bare runtime.** Ship the DisplayXR runtime plus the relevant display-vendor DP. End users get OpenXR + WebXR support out of the box; no spatial desktop. Lightest install. Best for products where the 3D display is one feature among many and you don't want to define a new desktop UX.
- **Runtime + reference shell.** Bundle the DisplayXR Shell from the DisplayXR project as your spatial-desktop layer. Brand the tray label via a sidecar manifest. Quickest path to a polished spatial-desktop experience without writing UX code.
- **Runtime + custom controller.** Write your own workspace controller against `XR_EXT_spatial_workspace` and `XR_EXT_app_launcher`. Differentiate your product line with a branded launcher, layout presets tailored to your form factor, custom chrome. Your controller gets the same first-class authentication as the reference shell.

**Q: Can I bundle the DisplayXR Shell with my product?**
Yes. The shell is distributed as a separate, redistributable binary. Bundle it with your installer or offer it as an optional download. Branding via the sidecar manifest (`display_name`, `vendor`, `icon_path`) means the tray UI carries your product name without any runtime modification.

**Q: How do I brand the tray and lifecycle UI for my product?**
Drop a `<controller-binary>.controller.json` sidecar manifest next to whichever workspace controller you ship — reference shell or custom. The runtime reads `display_name`, `vendor`, `version`, and `icon_path` and uses them in the tray submenu. No runtime rebuild required, no source access needed.

**Q: My product line spans multiple SKUs. Can I ship different controllers per SKU?**
Yes. The controller is selected via the runtime's `service.json` `workspace_binary` field. Your installer per SKU can write a different `service.json` that points at a different controller binary, or none at all. The runtime authenticates whatever it's configured to spawn.

**Q: How do third-party workspace controllers prove they're trustworthy?**
The runtime authenticates workspace activation by **orchestrator-PID match**: the controller is the binary the runtime's orchestrator was configured to spawn (via `service.json`'s `workspace_binary` field). The PID match — not the binary's `applicationName` string — is the credential. Your installer drops your controller into the runtime's directory and updates `service.json`; the runtime trusts whatever process starts as a result. No code-signing handshake, no shared-secret provisioning.

### For vertical integrators (CAD, medical, automotive HMI)

**Q: I want to ship a focused single-purpose 3D display experience — not a general-purpose desktop. Can I skip the workspace controller entirely?**
Yes. Run the bare runtime + your application. The runtime gives you OpenXR session management, native compositing, and display calibration. You skip the multi-app, windowing, and launcher complexity. This is the "kiosk" configuration.

**Q: I want a focused experience but with multi-app composition for my vertical (e.g., a medical imaging suite that runs imaging + chart + reference apps side-by-side). Can I write a controller for just that?**
Yes. A workspace controller is just a process that implements `XR_EXT_spatial_workspace` to define how it tiles and composites apps. Write a controller that hardcodes the layout your vertical needs. You keep full control of the UX without having to also implement a runtime.

**Q: Can I distribute my controller as a closed binary?**
Yes. The runtime ships under BSL-1.0; the extension headers are open. Your controller can be any license, any distribution model. The DisplayXR Shell is itself a closed-source proprietary product — it's the first proof point that the runtime supports closed controllers as first-class citizens.

### Why this exists / strategic context

**Q: Why an open runtime when the shell is proprietary?**
The 3D-display category needs platform standardization to grow. As long as every display ships its own non-portable stack, the addressable application catalog stays small and developers stay away. Opening the runtime layer — the part that's actually a platform — lets the catalog grow. The workspace layer is where vertical differentiation happens; that's a reasonable place for proprietary IP, including ours.

**Q: How does this compare to the headset-focused OpenXR runtimes?**
DisplayXR is built specifically for **glasses-free 3D displays**: lightfield panels, autostereo monitors, holographic displays. The shape of the problem is different from a headset (no HMD pose, no controllers, asymmetric projection per eye via Kooima math, eye-tracking-driven view selection). DisplayXR keeps the OpenXR contract that apps know but specializes the implementation for the 3D-display context. You can run a DisplayXR app and a headset OpenXR app from the same source — the differences are inside the runtime, not in the app code.

**Q: Why fork from Monado instead of starting from scratch?**
The conformance / state-tracker / extension-dispatch infrastructure in Monado represents years of OpenXR work that's worth carrying. We stripped 34 VR drivers, removed the Vulkan server compositor, replaced it with five native compositors (one per graphics API), and rebranded — but the OpenXR plumbing is still upstream-quality. We track upstream Monado and cherry-pick conformance fixes; see issue #47 for the strategy.

**Q: What's the licensing story?**
Runtime + bridge + extension headers: BSL-1.0 (permissive, similar to MIT). Reference DisplayXR Shell: proprietary, distributed separately. Third-party workspace controllers: any license the author chooses. The platform is permissively licensed by design — companies need to be able to ship runtime + bridge as part of their hardware product without legal review on every build.

**Q: What's on the near-term roadmap?**
- **Phase 2.A–2.H** (in progress): migrate launcher / chrome / capture / hit-test policy out of the runtime's compositor and into the workspace controller process, behind the as-yet-finalizing `XR_EXT_spatial_workspace.h` / `XR_EXT_app_launcher.h` headers.
- **Phase 3**: separate the shell repo from the runtime repo entirely. After Phase 3, the shell is just one of N possible controllers from a code-locality standpoint, not just from a runtime-policy standpoint.
- **macOS workspace controller**: Phase 6's spatial shell is Windows-only today. macOS port is on the M6 milestone.

**Q: How can I track progress / contribute?**
- Public runtime: `github.com/DisplayXR/displayxr-runtime` (releases, public issues, milestones).
- Extension headers: `github.com/DisplayXR/displayxr-extensions` (header-only, version-tagged).
- Reference shell binaries: `github.com/DisplayXR/displayxr-shell-releases` (user-facing bug reports).
- Demos: `github.com/DisplayXR/displayxr-demo-*` (one repo per demo, including a Gaussian-splatting renderer).

External contributions to the runtime go through PRs on the public `displayxr-runtime` repo; accepted PRs are mirrored into the private development repo via a documented apply script.
