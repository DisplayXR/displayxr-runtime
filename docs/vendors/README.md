# Vendor Integrations

DisplayXR is vendor-agnostic — any 3D-display vendor can integrate their hardware by implementing the [display processor interface](../specs/vendor/display-processor-interface.md) and following the [vendor integration guide](../guides/vendor-integration.md).

This directory holds **vendor-specific** documentation. Generic contracts that every vendor must follow live in `specs/vendor/` and `guides/`.

## Integrated vendors

| Vendor | Driver | Docs | APIs supported |
|---|---|---|---|
| **Leia SR** | `src/xrt/drivers/leia/` | [leia/](leia/) | D3D11, D3D12, OpenGL, Vulkan |
| **sim_display** | `src/xrt/drivers/sim_display/` | [sim_display/](sim_display/) | D3D11, D3D12, OpenGL, Metal |

`sim_display` is the reference simulation vendor — it ships with the runtime and renders side-by-side / anaglyph output to a normal 2D window so contributors can develop without 3D-display hardware.

## Adding a new vendor

1. Read [guides/vendor-integration.md](../guides/vendor-integration.md) — the end-to-end walkthrough.
2. Implement the [display processor vtable](../specs/vendor/display-processor-interface.md) for each graphics API you support.
3. Decide your [eye-tracking mode](../specs/vendor/eye-tracking-modes.md) (MANAGED or MANUAL).
4. Add `docs/vendors/<your-vendor>/` with a `README.md` describing your integration; link any internals docs (weaver, transparency model, etc.) from there.
5. The relevant ADRs are [ADR-003](../adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) (vendor isolation), [ADR-007](../adr/ADR-007-compositor-never-weaves.md) (compositor/DP boundary), and [ADR-015](../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md) (multi-display routing).
