# Local 2D/3D Zones — Per-Window 3D Without Switching the Whole Screen

**Status**: Phase 0 runtime side IMPLEMENTED (June 2026) — D3D11 DP vtable additions (`get_local_zone_caps` / `publish_local_zone_mask` / `clear_local_zone_mask`, appended per ADR-020), the compositor per-frame publish path, hwGrid caps wiring, and the sim_display test double (`SIM_DISPLAY_ZONE_GRID` / `SIM_DISPLAY_ZONE_DUMP`). The authoring extension shipped earlier via #439 Phases 1–2. **Revised from the original draft:** the in-process D3D11 vtable contract is *stateless publish* (runtime-owned mask texture, SRV passed per publish) instead of the DP-allocated create/publish/clear/destroy lifecycle below — see "DP vtable contract". Remaining: first-vendor (Leia) 1×1 implementation, occlusion subtraction, per-zone hardware.
**Scope**: regular Windows desktop, no shell, single or multiple 3D apps in normal windows. Shell-mode generalization is a superset of this and not covered here.
**Audience**: DisplayXR runtime contributors; vendors implementing the display processor (DP) vtable.

> **Relationship to compositor 2D/3D compositing.** This note covers the **hardware-consumer leg** of a shared mask: publishing a screen-space 3D-zone mask to the vendor DP → panel firmware so the *physical switchable-lens cells* track which window regions are 3D. The complementary **compositor-consumer leg** — the runtime software-compositing flat 2D over weaved 3D inside an app's surface, driven by the *same* authored alpha mask — is specified in [unified-2d-3d-compositing.md](unified-2d-3d-compositing.md). The two are one artifact with two readers and **must agree** (a weaved pixel needs a 3D lens cell over it; a flat pixel needs a flat cell). The `XR_EXT_local_3d_zone` authoring API below is shared by both legs; the unifying spec adds the second consumer behind it rather than inventing a second mask.

## Problem

Today's switchable-lens displays expose a single global panel state: either the whole panel is in 3D or none of it is. Any connected client requesting 3D pins the entire panel to 3D. This forces 3D apps into either fullscreen or "the whole desktop looks distorted while a small 3D window is open."

We want: an app rendering a normal Win32 window can have *its window region* of the panel in 3D, while the rest of the desktop stays 2D — without changing how DWM composes, and without requiring any shell or window manager to mediate.

The vendor's display hardware may support fine-grained per-zone switching, coarse-grained per-zone switching, or only the legacy global on/off mode. The runtime API should be the same in all three cases — only the vendor's DP implementation distinguishes.

## Approach in one sentence

The app's compositor pre-weaves into its own swap chain (same as today). The runtime publishes a screen-space **3D zone mask** — a binary GPU texture in client-window pixel space — to the display processor each frame via a new vtable method. The vendor's DP implementation translates that mask (and the masks of other clients, if any) into whatever hardware zone mechanism it supports, including the degenerate 1×1 case of a global on/off switch.

On hardware with a 1×1 zone grid (today's panels), the model degrades exactly to today's global on/off — bit-compatible from the app's perspective.

## Responsibility split

| Layer | Owns |
|---|---|
| **App** | Multiview rendering. Final 2D composition into its own window. Declaring which window regions it wants in 3D. |
| **DisplayXR runtime** | Pre-weaving multiview → interlaced in the app's swap chain (unchanged). Mapping client mask → screen space. Occlusion subtraction via Z-order walk. Calling the DP vtable to publish the result. |
| **Vendor (DP implementation)** | Translating the runtime-supplied mask + screen-rect into hardware zone state. Any cross-process arbitration across multiple connected clients. Any vendor-specific filtering, hysteresis, debouncing. Pushing to firmware. |
| **DWM** | Unchanged. Composes the desktop opaquely. Never sees the mask. |
| **Panel firmware** | Per-cell lens/barrier state from whatever channel the vendor uses. Outside the spec. |

Two firm boundaries: (1) DWM is the immovable composition layer, so weaving stays per-swap-chain and the mask is a sideband signal. (2) The DP vtable is the only contact point between runtime and vendor — vendor internals (services, kernel drivers, IPC topology, arbitration policy) are entirely vendor's choice.

## Architecture

```
App process                                Vendor DP impl              Panel
+----------------------+
| App 2D UI code       |
|   |                  |
|   |  multiview       |
|   v  content         |
| [Runtime compositor] |--- swap chain --> DWM ----> scanout ---> [panel pixels]
|   pre-weaves         |
|   into rect of       |
|   swap chain         |
|                      |
| [Runtime zone-mask   |--- DP vtable call: publish_zone_mask(tex, rect)
|  publisher]          |       (shared GPU texture + window screen rect)
+----------------------+                                |
                                                        v
                                            [vendor's arbitration +
                                             downsample + firmware push]
                                                        |
                                                        v
                                          [per-cell hardware lens state]
```

Two channels per app: (1) the swap chain DWM sees (interlaced inside 3D rects, mono outside); (2) a sideband mask published through the DP vtable. They must be consistent each frame — the runtime owns making that true.

---

## DP vtable contract (as implemented — Phase 0, D3D11)

This is the only surface between runtime and vendor. Three methods appended to `xrt_display_processor_d3d11` (`src/xrt/include/xrt/xrt_display_processor_d3d11.h`, slots 12–14; shared caps struct in `xrt_display_zones.h`), append-only within the ABI major per ADR-020 — older plug-ins report a smaller `struct_size` and the runtime treats the slots as absent (legacy DP).

> **Design revision vs. the original draft.** The draft had the DP allocate a shared mask texture behind a create/publish/clear/destroy handle lifecycle — a shape motivated by cross-process sharing. The shipped `XR_EXT_local_3d_zone` compositor consumer (#439) already owns a window-sized R8_UNORM staged mask on the session's own in-process D3D11 device, so the in-process contract is **stateless publish**: the runtime passes the mask SRV per publish and the DP samples/copies it during the call (the immediate context serializes against the runtime's writes; the DP must not hold the SRV past return). DP-side lifecycle methods can be appended later when a cross-process leg (IPC/service zone masks) needs real sharing primitives.

### Capability query

```c
struct xrt_dp_local_zone_caps {        // xrt_display_zones.h — fixed-width, struct_size-headed (ADR-020)
    uint32_t struct_size;              // pre-set by the CALLER (runtime); DP writes only fields within
    uint32_t supported;                // 0 on DPs that don't implement local zones
    uint32_t zone_grid_width;          // hardware cells across (1 = global on/off)
    uint32_t zone_grid_height;         // hardware cells down
    uint32_t max_mask_width;           // upper bound on the published mask resolution (0 = no preference)
    uint32_t max_mask_height;
    uint32_t max_update_hz;            // 0 = unlimited; vendor's preferred max mask refresh (advisory in Phase 0)
};

bool (*get_local_zone_caps)(struct xrt_display_processor_d3d11 *xdp,
                            struct xrt_dp_local_zone_caps *out_caps);
```

Three meaningful return shapes:

- slot absent / NULL / `supported = 0`: legacy DP. Runtime keeps the existing global `request_display_mode` path and never calls the publish methods. `xrGetLocal3DZoneCapabilitiesEXT` reports `hardwareZoneGrid = 0×0` (the compositor consumer still works — the mask composites, it just can't drive the panel).
- `supported = 1, zone_grid = 1×1`: vendor implements the new API but the hardware is single-zone. OR-union of all client masks collapses to "any client requesting any 3D anywhere → screen is 3D" — identical to today's bool arbitration.
- `supported = 1, zone_grid > 1×1`: real local zones. Same runtime path; vendor does the heavy lifting.

### Per-client mask publish (stateless)

```c
// Per-frame publish. The runtime owns the mask texture (the staged
// XR_EXT_local_3d_zone snapshot — R8_UNORM, client-window pixels, non-zero =
// 3D) and passes an SRV on the session's own D3D11 device. screen_x/y/w/h
// anchor the mask's pixel space on the panel (physical pixels, post-DPI
// client rect). seq is a monotonic per-session publish counter for
// vendor-side coalescing. The DP samples or copies DURING the call.
bool (*publish_local_zone_mask)(struct xrt_display_processor_d3d11 *xdp,
                                void *d3d11_context, void *mask_srv,
                                uint32_t mask_width, uint32_t mask_height,
                                int32_t screen_x, int32_t screen_y,
                                uint32_t screen_w, uint32_t screen_h,
                                uint64_t seq);

// Equivalent to "this client is fully 2D." Cheaper than publishing an empty
// mask. Called on the active→inactive edge (mask destroyed, session ends).
bool (*clear_local_zone_mask)(struct xrt_display_processor_d3d11 *xdp);
```

Mask values: 1 channel; the vendor must treat **any non-zero value as 3D** (the authored mask has fractional anti-aliased edges — the OR rule makes them conservative). Resolution is the authored mask resolution in **client-window pixels**; the runtime tells the vendor where that pixel space maps onto the screen via the `screen_*` args, and republishes every frame while a mask is active so the anchor tracks window moves/resizes (vendors coalesce per `max_update_hz`).

### What the DP vtable specifies

- The texture format, sync primitive, and lifecycle.
- The coordinate space: client mask is window-pixels; screen rect is physical screen pixels.
- The downsampling-and-arbitration rule: **any non-zero client pixel in a hardware cell → cell is 3D** (OR union). This rule is what makes the 1×1 case bit-compatible with today's bool arbitration.
- That the vendor must preserve any existing global force-2D admin override path (tray utility, OS lock screen, etc.) as a supersede of the union result.

### What the DP vtable does NOT specify

- Whether the vendor uses a service process, in-process arbitration, kernel driver, or some other mechanism to handle multiple clients. Single-binary vendors may implement everything in-process; multi-process vendors will likely arbitrate in their own service. Both are conformant.
- The on-wire transport for the shared texture between runtime and vendor service (if one exists).
- How fast the vendor pushes firmware updates, or how the firmware actually toggles the panel.
- Any hysteresis, debouncing, or smoothing the vendor applies before driving the panel.
- How the vendor distinguishes DisplayXR clients from other consumers of its DP (if any).

The vendor is free to do anything that produces the right user-visible result.

### Backwards compat invariants the DP must preserve

- **Global admin force-2D**: if the vendor's stack supports a tray/admin override that pins to 2D today, that override must continue to supersede any union of masks.
- **OS lock screen** (Windows session lock, etc.): vendor's existing 2D pinning during lock continues unchanged.
- **Client disconnect / mask destroy**: client's contribution disappears from the union next frame.
- **Legacy `request_display_mode`**: still supported by DPs that don't implement local zones. Runtime falls back to it. Vendors are not required to support both paths simultaneously — local-zone-capable DPs may treat `request_display_mode(want_3d=true)` as equivalent to a publish of an all-1 mask covering the full screen.

---

## DisplayXR runtime piece

Everything above the DP vtable that we own in this repo.

### New OpenXR extension: `XR_EXT_local_3d_zone`

Thin shim over the DP vtable. Lets OpenXR apps express "this region of my window is 3D" without touching vendor APIs directly.

```c
// Capability query.
XrResult xrGetLocal3DZoneCapabilitiesEXT(
    XrSession session,
    XrLocal3DZoneCapabilitiesEXT* caps);

// Create a mask bound to the session's window (HWND known via XR_EXT_win32_window_binding).
XrResult xrCreateLocal3DZoneMaskEXT(
    XrSession session,
    const XrLocal3DZoneMaskCreateInfoEXT* info,
    XrLocal3DZoneMaskEXT* outMask);

// Tier 1 — whole-window convenience.
XrResult xrSetLocal3DZoneWholeWindowEXT(
    XrLocal3DZoneMaskEXT mask,
    XrBool32 enable3D);

// Tier 2 — rect list. Runtime rasterizes into the mask texture for the app.
XrResult xrSetLocal3DZoneFromRectsEXT(
    XrLocal3DZoneMaskEXT mask,
    uint32_t rectCount,
    const XrRect2Di* rects);                      // in client-window pixels

// Tier 3 — render-target access for dynamic / freeform masks.
// Returned binding is graphics-API-typed via sibling extensions
// (XR_EXT_local_3d_zone_d3d11, _d3d12, _vulkan, _opengl, _metal).
XrResult xrAcquireLocal3DZoneRenderTargetEXT(
    XrLocal3DZoneMaskEXT mask,
    void* outBinding);

// Submit current state. Runtime applies occlusion, computes screen rect,
// signals fence, calls DP vtable publish.
XrResult xrSubmitLocal3DZoneEXT(
    XrLocal3DZoneMaskEXT mask);

XrResult xrDestroyLocal3DZoneMaskEXT(XrLocal3DZoneMaskEXT mask);
```

Tiered API ergonomics:

- **Tier 1** (most apps): one call at startup. Whole window 3D. Zero per-frame work, zero textures the app sees.
- **Tier 2** (apps with a static 3D viewport in 2D UI): one call per layout change. Runtime rasterizes rects into the mask texture via a small built-in shader. No app GPU code touches the mask.
- **Tier 3** (truly dynamic): app gets the render-target view and draws into the mask as part of its normal render pipeline. Pays a small extra render pass per frame; nothing else.

In all three tiers the wire-level primitive — what reaches the DP — is the same shared GPU texture. The tiers are just three ways for the app to write into it.

### Runtime per-frame work (for each session with an active mask)

On every `xrEndFrame` (as implemented: `d3d11_sync_zone_mask_to_dp` in `comp_d3d11_compositor.cpp`, under the same `c->mutex` scope as the zone composite so the two mask consumers see identical state):

1. **Client screen rect**: `GetClientRect(hwnd)` + `ClientToScreen` each frame (physical pixels, same convention as `get_window_metrics`).
2. **Z-order occlusion** — *deferred (Phase-0 publishes the un-occluded mask)*: walk `GetWindow(hwnd, GW_HWNDPREV)` up to the top; intersect each window above ours and accumulate a "visible region in client-window pixels."
3. **Apply occlusion to the client mask** — *deferred with 2*: small shader ANDs the staged mask with the rasterized visible region. On 1×1-grid hardware the deferral changes nothing (the union result is identical); it starts mattering when overlapping windows meet per-zone hardware.
4. **Publish**: bump the per-session seq, call `publish_local_zone_mask(dp, ctx, staged_srv, mask_w, mask_h, screen rect, seq)`. On the active→inactive edge (mask destroyed / teardown), call `clear_local_zone_mask` once.

Occlusion is the runtime's responsibility, not the vendor's. The DP vtable sees only "this client wants this region in screen space, post-occlusion."

### Multiple 3D apps on the same desktop

Falls out naturally. Each app's runtime instance has its own `xrt_dp_local_zone_mask` and publishes its own occluded mask. The vendor's DP implementation OR-unions across all connected clients. Two non-overlapping 3D windows → two non-overlapping panel zones. Overlapping windows → the front one's mask wins for the overlap (because the back one subtracted those pixels during its own occlusion pass).

Whether the vendor's arbitration runs in-process per app or in a centralized vendor service is invisible to the runtime — that's the whole point of the DP vtable contract.

### Fallback path

If `xrt_dp_get_local_zone_caps(dp).supported == false`:

- `xrGetLocal3DZoneCapabilitiesEXT` reports unsupported. Most apps will then either fall back to today's global request flow (existing `xrRequestDisplayRenderingModeEXT` path on legacy `XR_EXT_display_info`) or stay 2D.
- A convenience compatibility shim: `xrSetLocal3DZoneWholeWindowEXT(true)` on a legacy DP transparently calls the legacy global-3D path. `false` calls the legacy global-2D path. Apps that only need whole-window 3D need no version-specific code.

### What the runtime does NOT do

- Allocate or manage the shared GPU texture for the mask. The DP creates it on `xrt_dp_create_local_zone_mask` and hands the runtime a writable view. (Rationale: format, sharing primitive, and lifetime are vendor- and graphics-API-specific.)
- Implement cross-process arbitration. That's vendor-side, behind the DP vtable.
- Touch DWM in any way.
- Special-case shell vs. no-shell. The shell, when it exists, is just another client publishing its own mask via the same path.

---

## App-side usage patterns

**Whole-window 3D app** (e.g. today's `cube_handle_d3d11_win`):
```c
XrLocal3DZoneMaskEXT mask;
xrCreateLocal3DZoneMaskEXT(session, &createInfo, &mask);
xrSetLocal3DZoneWholeWindowEXT(mask, XR_TRUE);
xrSubmitLocal3DZoneEXT(mask);                  // once at startup
// ... no per-frame mask work; runtime republishes each frame.
```

**Static 3D viewport in a 2D UI**:
```c
// On window resize / viewport move:
XrRect2Di rect = { {viewport_x, viewport_y}, {viewport_w, viewport_h} };
xrSetLocal3DZoneFromRectsEXT(mask, 1, &rect);
xrSubmitLocal3DZoneEXT(mask);
```

**Animated / freeform mask** (Tier 3):
```c
// Per-frame, inside the app's render loop:
XrLocal3DZoneRenderTargetD3D11EXT rt;
xrAcquireLocal3DZoneRenderTargetEXT(mask, &rt);
// Draw shapes into rt.renderTargetView using app's normal renderer.
xrSubmitLocal3DZoneEXT(mask);
```

---

## Open questions

1. **Mask resolution defaults**: what's a reasonable upper bound on mask size before vendors push back? Suggested default in spec is `window_w/8 × window_h/8` clamped to `[16×16 .. 1024×1024]`. Negotiable per vendor via `max_mask_width/height` caps.
2. **Update cadence**: per-frame is the simplest contract. The `max_update_hz` cap lets vendors throttle if their firmware can't sustain submission rate. Should the runtime soft-cap based on this, or pass everything through and let the vendor coalesce?
3. **Hysteresis / debounce at zone boundaries**: when the user drags a 3D window, hardware cells flicker as the window moves. Vendor's responsibility — but worth documenting expected behavior so the runtime can choose whether to add its own debouncing on top.
4. **Cursor handling**: the hardware cursor sits on top of the swap chain and is 2D. When it passes over a 3D zone, it can show distortion. Three options to choose between: accept the artifact (smallest); runtime carves a small 2D rect around `GetCursorPos()` (medium); vendor handles cursor compensation in firmware (best, but vendor-dependent).
5. **Sub-pixel screen rect alignment**: window screen rects may not align with hardware zone-grid cells. The OR-downsample rule already handles this conservatively (any 3D pixel in a cell → cell is 3D), but some vendors may want a tighter contract. Keep loose for now.

---

## Phasing

**Phase 0 — DP vtable additions land in the runtime, on top of existing hardware.**
*Runtime side DONE (June 2026): D3D11 vtable slots 12–14, compositor per-frame publish/clear, hwGrid caps wiring, sim_display test double (`SIM_DISPLAY_ZONE_GRID=WxH` simulated grid + `SIM_DISPLAY_ZONE_DUMP=1` OR-downsampled per-cell readback logging — the hardware-free end-to-end check).* Remaining: first-vendor (Leia) DP implements `get_local_zone_caps` reporting `supported = 1, zone_grid = 1×1` and OR-collapses publishes to today's global on/off path. Existing apps unaffected. Validates the API surface before any per-zone hardware exists.

**Phase 1 — runtime ships `XR_EXT_local_3d_zone`.**
*Authoring API + compositor consumer SHIPPED via #439 Phases 1–2 (ahead of this leg).* Remaining here: occlusion subtraction (deferred from Phase 0) in preparation for Phase 2. Any vendor wanting to participate implements the DP vtable additions; the bar is low (1×1 grid + the new vtable methods is enough).

**Phase 2 — per-zone hardware lands** (from any vendor).
That vendor's DP reports a real `zone_grid_width/height`. Same client code now produces per-window 3D zones. No app changes; runtime changes are confined to capability reporting and any optional vendor-specific tuning.

**Phase 3 — DisplayXR Shell adopts the same API.**
Shell becomes one more client publishing a mask; multi-app per-window 3D in shell mode falls out of the existing primitives. No shell-specific DP path required.

---

## Cross-references

- DP vtable: `src/xrt/include/xrt/xrt_display_processor_d3d11.h` — the local-zone methods (slots 12–14) sit after `set_atlas_encoding`; shared caps struct in `xrt_display_zones.h`. The base/Vulkan and other per-API vtables get the same append when their consumer legs land (zone masks are in-process D3D11 only today).
- Vendor integration guide: `docs/guides/vendor-plugin-onboarding.md` — to be extended with the local-zone vtable methods once this spec lands.
- Vendor-initiated state change events (today): `comp_d3d11_service.cpp:10180–10230`, `oxr_session.c:968` — the polling/event pattern that the new local-zone path coexists with, not replaces.
- ADR-014 (shell owns rendering mode): generalizes in the zone world — the shell becomes one mask publisher among many rather than the sole policy holder.
- `docs/specs/extensions/XR_EXT_display_info.md`: candidate place for `localZoneCapable` + grid dimensions to surface to apps that don't take the full `XR_EXT_local_3d_zone` extension.
