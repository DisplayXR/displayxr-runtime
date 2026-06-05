---
status: Accepted
date: 2026-06-04
source: "#409"
---
# ADR-021: Color Management & the Encoding-State Invariant

## Context

The runtime's color handling grew ad-hoc — each compositor path adopted whatever
colorspace strategy fit the apps it happened to meet. The recent sRGB/gamma fixes
([#407](https://github.com/DisplayXR/displayxr-runtime/pull/407) GL,
[#408](https://github.com/DisplayXR/displayxr-runtime/pull/408) D3D11/D3D12/VK/Metal)
corrected a real ~2.2×-too-dark bug in the in-process path, but the investigation
([#409](https://github.com/DisplayXR/displayxr-runtime/issues/409)) showed the three
paths each encode a *different* convention, and that one of them carries the same
half-conversion #407 fixed — latent only because no shipping app exercises it.

There was no written contract for what colorspace state a texture holds at each hop,
so every fix was reactive — and worse, the reactive fixes were tuned to what one
vendor's display processor happened to want. That inverts DisplayXR's core
dependency direction: **the runtime defines vendor-neutral conventions; the display
processor adapts to them** (ADR-003, ADR-007, ADR-019). This ADR establishes the
color contract on those terms.

### The bug class, generalized

A texture holds pixels in one of two **encoding states**:

- **Linear** (scene-referred / radiance) — proportional to light. Blending, filtering,
  MSAA resolve, and mipmap generation are *only correct here*.
- **Encoded** (display-referred, sRGB/gamma) — in the standard display transfer
  function; what a display physically wants.

A texture's **format declares which**: a `*_SRGB` format ⟹ encoded (the GPU
auto-decodes on sample, auto-encodes on render-target write); a UNORM / float format
⟹ linear. Every bug we chased was the same shape — a **half-conversion**: a decode
(format said sRGB) with no matching encode, or vice versa.

## Decision

### 1. The matched-pair invariant (the central rule)

> **Colorspace conversions come in matched pairs (decode … encode) or not at all
> (passthrough) — never half.**

This is path-independent and model-independent. Any place that decodes (samples an sRGB
SRV, calls a `srgb_to_linear`) must have a corresponding encode before the bytes leave
for the next canonical space, or it must not decode at all.

### 2. Standard transfer function; no vendor curves in the runtime

The canonical encoded state is **standard sRGB**. The runtime's compositor/shader code
must never use a vendor-specific transfer function. The existing
`linear_to_srgb()` in `d3d11_service_shaders.h` bakes a vendor-specific power-law curve
into shared runtime code — a vendor-isolation violation; it is to be removed. If a
vendor's panel needs a non-sRGB curve, that mapping lives **inside that vendor's DP**,
applied after the runtime hands off standard bytes.

When a path does decode/encode, **both sides of the pair use the same (sRGB) curve.**
Pairing an sRGB decode with a different-curve encode is a half-broken round-trip
(persistent tint), not a clean one.

### 3. Two canonical spaces; the DP-handoff space is *declared by the DP*

```
app render → [swapchain] → sample → [compose / atlas] → process_atlas() → panel
                         ^^^^^^^               ^^^^^^^^^^^^^^^^
                      COMPOSE space          DP-HANDOFF space
```

- **Compose space** is DisplayXR-internal and vendor-neutral (model choice in §5).
- **DP-handoff space is a negotiated contract, not a fixed value.** Two declarations
  define it:
  1. **DP capability (static):** the DP declares which encoding state(s) it can accept —
     `LINEAR`, `ENCODED`, or `EITHER`.
  2. **Runtime intent (per-frame):** the runtime declares the encoding state of the atlas
     it is actually sending via a dedicated `set_atlas_encoding(LINEAR|ENCODED)` call made
     immediately before `process_atlas()`. (This is conveyed out-of-band, *not* by
     overloading the `process_atlas` `format` argument: that argument turned out **not** to
     be dead — some DPs feed it straight to their weaver's `setInputViewTexture` as the real
     texture format — so it stays the real format and the encoding rides a separate,
     append-only vtable slot. See *Consequences*.)

  The runtime guarantees it sends an encoding the DP can accept; if its compose space
  doesn't match a capability-restricted DP, the runtime converts compose→accepted as a
  matched-pair step before handoff. The DP then guarantees panel-correct (encoded) output
  to the display, converting internally as needed.

  This is the key correction over a naive "handoff = encoded" rule, which was
  reverse-engineered from one vendor's weaver. A future vendor whose weaver consumes only
  linear input declares `LINEAR`; one that consumes only encoded declares `ENCODED`; the
  runtime serves both with no compositor change.

### 4. DPs that accept `EITHER` make the contract cheap

A display processor whose weaver exposes an explicit input/output sRGB-conversion control
(decode-before-weave and/or encode-after-weave) can accept **`EITHER`** encoding *and*
perform the final output encode itself — the encoding is selected by an API knob, not
inferred from the texture format. This is the common case for production weavers, and it
collapses the boundary problem:

- The runtime sends its **native compose space** to such a DP and never converts at the
  boundary; the DP configures its weaver to match the runtime's per-frame declaration:
  - atlas **encoded** → weave as-is, output encoded (today's behavior), or decode→weave
    in linear→re-encode.
  - atlas **linear** → weave in linear (correct blend/sampling math) and **encode on
    output**. The matched encode lives in the DP's weaver, so the runtime needs **no
    encode shader** for Model B.
- This is why deleting `linear_to_srgb()` from the runtime (§2) is not just legal but
  correct: the encode belongs in the DP.

For an `EITHER`-capable DP, the runtime's A-vs-B choice (§5) is purely an internal
correctness decision with **zero** boundary-conversion cost. The concrete control
exposed by a given vendor's weaver is documented under that vendor's docs
(`docs/vendors/<vendor>/`), never here.

### 5. Compose-space model: Model A baseline, Model B sanctioned future

The compose-space encoding is an **internal** DisplayXR decision about compositing
correctness, now decoupled from whatever any DP wants:

- **Model A (baseline, today):** compose in **encoded** space (passthrough). Apps in
  this ecosystem write display-referred bytes into both sRGB and UNORM swapchains, so
  passthrough is the coherent baseline. The in-process path is already here (post
  #407/#408); the service/workspace path must drop the unmatched decode on its
  sRGB-client branch (use the UNORM SRV / raw bytes). Until then that branch is guarded
  with a one-shot warning in `multi_compositor_render`.
- **Model B (sanctioned future):** compose in **linear** space — decode iff sRGB →
  compose linear → convert to the DP's declared handoff state. This is the *correct*
  model for alpha compositing, premultiplied-alpha math, and the compose-under-background
  transparency feature, all wrong in non-linear space.

  Key property: **B degenerates to A (passthrough) wherever there is nothing to blend**
  (decode-then-immediately-reconvert is the identity). So B only costs more on the
  **workspace/service compose path** where surfaces overlap — exactly where the
  transparency feature lives. It is gated on that feature needing correct blending.

### 6. Format is the source of truth for a swapchain's encoding state

`*_SRGB` ⟺ encoded; UNORM / float ⟺ linear. This requires apps to declare honestly
(request an sRGB swapchain and store correctly-encoded pixels). Some test apps violate
this today (encoded bytes in UNORM swapchains); see *Consequences*.

## Consequences

- **Vendor isolation preserved.** Color conventions are DisplayXR's; the DP adapts via
  a declaration. Adding a vendor with a different input-encoding need requires zero
  compositor changes — only the DP declares its preference.
- **Two new append-only DP-vtable slots** (ADR-020: `struct_size`-gated, **absent ⟹
  `ENCODED`** so older plug-ins keep their passthrough behavior):
  1. `get_handoff_color_capability()` — the DP's static accepted encoding (`LINEAR` /
     `ENCODED` / `EITHER`). Production weavers with a configurable conversion control
     declare **`EITHER`**.
  2. `set_atlas_encoding(LINEAR|ENCODED)` — the runtime's per-frame intent, called just
     before `process_atlas()`. It is a *separate* slot rather than a `process_atlas`
     argument because the `format` argument is **not** dead in practice — some weavers
     consume it as the real input-texture format — so it stays the real format and the
     encoding rides out-of-band. Both slots are append-only ⟹ **no ABI major bump**, and
     the change is back-compatible in both directions (old-runtime+new-plug-in and
     new-runtime+old-plug-in both fall to Model A).
- **Vendor DP work (per-vendor, in each plug-in repo):** a vendor whose weaver supports
  configurable conversion should (1) wire that control to the runtime's declared atlas
  encoding (encode on output whenever input is linear), (2) expose it for **every
  graphics-API variant** it ships, and (3) declare its capability. Concrete per-vendor
  steps live in that vendor's docs, not in this ADR.
- **One rule for all paths.** "Is this path balanced?" becomes a mechanical check
  against the per-hop encoding table — and a regression test, not a code review.
- **The latent service-path bug is documented and guarded**, with a defined fix: under
  Model A, passthrough the sRGB-SRV branch; the conversion-to-handoff machinery (§4) is
  what ultimately replaces the ad-hoc branch.
- **Vendor curve removed.** `linear_to_srgb()` (a vendor power-law) is deleted from
  runtime shaders; any panel curve is DP-internal.
- **App colorspace contract becomes load-bearing.** Honest declaration is required for
  Model B; a transitional per-app `treat-as-encoded` override is the escape hatch for
  legacy/test apps that put encoded bytes in UNORM. Our own test apps should migrate to
  honest sRGB swapchains.
- **Verification gap closed.** The cube test apps gained `DXR_SWAPCHAIN_ENCODING=srgb|unorm`
  + `DXR_TRUE_LINEAR` modes (a true-linear-into-UNORM source), and `sim_display`'s D3D11
  variant declares `EITHER` + encodes on `LINEAR` — the in-repo DP test double for the
  encode-at-handoff direction. Matrix: `{sRGB, UNORM-encoded, true-linear} × {single, multi}
  × {in-process, IPC, workspace}` (`test_apps/COLOR_REGRESSION_MATRIX.md`).
- **HDR / wide-gamut**, if it lands on the roadmap, forces Model B with a float16 linear
  compose space and would move B from "gated future" to "now."

### As shipped (runtime #419 + leia-plugin)

Model B landed on the **D3D11 workspace/service compose path** with two refinements the
implementation forced:

- **Honest-sRGB gate (load-bearing).** Model B engages only when the DP accepts linear, ≥2
  layers composite, **and every contributing client is a genuine `*_SRGB` swapchain**. A
  UNORM swapchain is ambiguous (encoded-in-UNORM vs already-linear-in-UNORM — the two can't
  be told apart from the format), so UNORM clients stay on Model A passthrough. This is §6
  "format is the source of truth" made operational: only honest-sRGB content is decoded.
- **Runtime chrome + backdrop linearize too.** Under Model B the whole atlas is declared
  `LINEAR`, so everything the *runtime* composites (window chrome, focus glow, cursor, and
  the `#1a1a1a` backdrop clear) — all display-referred — must also be decoded to linear, or
  the DP's single output encode double-encodes them. The blit shader applies the standard
  sRGB decode on every output (content + chrome) and the backdrop is cleared to the linear
  value; the DP performs the one matched encode.
- The mechanism is otherwise as in §3–§5: raw-sample → linear compose → `set_atlas_encoding(LINEAR)`
  → the weaver's output encode. In-process paths and UNORM workspaces are unchanged Model A.

## Encoding state at each hop (current baseline = Model A; DP configured for encoded passthrough)

| Hop | In-process | IPC / service | Workspace / shell |
|---|---|---|---|
| App swapchain | encoded (in sRGB or UNORM) | encoded | encoded |
| Sample into compose | passthrough (UNORM SRV / skip-decode) | passthrough (raw copy) | passthrough (raw copy → TYPELESS atlas) |
| Compose / atlas | encoded | encoded | encoded (UNORM SRV branch) / **linear (sRGB SRV branch — unmatched decode, guarded, to be removed)** |
| Compose → handoff (per DP decl.) | passthrough (DP accepts encoded) | passthrough | passthrough except the sRGB-SRV branch |
| DP handoff | **encoded** ✓ | **encoded** ✓ | **encoded** ✓ except the sRGB-SRV branch |

Under Model B (future) the "Compose / atlas" row becomes **linear** on the
workspace/service path, and the "Compose → handoff" row carries the matched conversion
to whatever the DP declared (encode-to-sRGB for an `ENCODED` DP; passthrough for a
`LINEAR` DP). The in-process row is unchanged (passthrough == A == B with nothing to
blend).
