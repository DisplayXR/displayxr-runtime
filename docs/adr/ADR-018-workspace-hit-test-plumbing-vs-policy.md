---
status: Accepted
date: 2026-05-20
---
# ADR-018: Workspace Hit-Test Is Plumbing; Drag/Resize/Cursor Policy Is the Controller's

## Context

The workspace runtime currently does spatial raycasting in `workspace_raycast_hit_test` (`comp_d3d11_service.cpp`) to determine which tile / chrome region / edge the cursor sits over. Six callsites consume the result:

1. **Per-frame cursor sprite render** (line ~7000) — the runtime draws a 3D cursor at the hit's Z-depth.
2. **LMB-pressed handler** (line ~7150) — runtime starts edge-resize or title-bar drag state machines (`mc->resize`, `mc->title_drag`).
3. **RMB-pressed handler** (line ~7605) — runtime starts a title-bar rotation drag (`mc->title_rmb_drag`).
4. **POINTER event publish** (line ~14200) — runtime enriches a `WORKSPACE_PUBLIC_EVENT_POINTER` event with `hit_region`, `hit_client_id`, `local_uv`, `chrome_region_id` before delivering it to the controller.
5. **MOTION event publish** (line ~14240) — same enrichment on per-frame mouse-motion events.
6. **Public extension API `xrWorkspaceHitTestEXT`** (line ~15240) — controller queries hit-test directly.

The DisplayXR Shell (and any future workspace controller) **consumes** the runtime's hit-test output via `XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT` (`currentClientId`, `currentChromeRegionId`) to drive chrome button hover state. It does not currently do its own raycast.

The [Controllers own motion](../../) memory and the broader separation-of-concerns rule say "runtime is plumbing only; controllers own interactive policy." Read literally, the runtime computing hit-test on every cursor motion looks like a policy violation. This ADR clarifies why it isn't, and identifies what *is* the actual layering violation in this area.

## Decision

**Hit-test computation stays in the runtime as plumbing**, exposed to the controller via:

- `WORKSPACE_PUBLIC_EVENT_POINTER` / `_MOTION` event enrichment (passive — controller reads it if useful).
- `XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT` notification on transitions.
- `xrWorkspaceHitTestEXT` synchronous query API.

Rationale:

- The runtime owns the source data (per-eye tile pixel layout, cursor in window coords, predicted eye positions). Computing the raycast at the data is efficient — no IPC round-trips to publish layout + cursor + eye-pos to the controller and read back a hit result.
- The raycast itself is a pure function over data the runtime already has. It does not *decide* what to do with the result — that's policy.
- Controllers retain the freedom to ignore the runtime's hit-test and compute their own (they have everything they need via their own session: `xrLocateViews`, the layout they sent via `set_chrome_layout`, and they can hook the cursor themselves). The runtime's published result is a convenience, not a constraint.

**Interactive policy belongs in the controller.** Specifically, the following pieces are still in the runtime today and represent the real layering violation; they are slated for migration in follow-up work:

1. **Drag state machine** (`mc->title_drag` — title-bar grip drag, mouse-delta-to-pose conversion, drag-end snap policy).
2. **Resize state machine** (`mc->resize` — which edges resize, axis-locked vs corner, minimum-size enforcement, post-resize layout snap).
3. **Rotation drag** (`mc->title_rmb_drag` — RMB-on-grip rotates the tile around its center; how mouse delta maps to yaw/pitch).
4. **3D cursor sprite rendering** — what cursor shape to show (arrow / sizewe / sizenwse / sizeall), where it sits in Z-depth (currently uses raycast hit z), whether it's visible at all.

These are *policy* — they decide what the user's input *does*. Per ADR-016 they should live in the controller, with the runtime providing only the inputs (cursor pose in workspace space, hit results, button state events) and accepting the outputs (window pose updates via `set_pose` IPC, requested cursor shape via a future extension API).

## Consequences

### What this enables now

- The runtime keeps the hit-test entrypoints documented above. No code change.
- The "compositor is doing things the controller should be doing" critique is bounded to the four policy items, not to the existence of `workspace_raycast_hit_test`.

### Follow-up work (not in this ADR)

- **Drag/resize/rotation state machines → controller.** Requires controller-side per-frame tick that consumes raw button events + hit results, emits window-pose updates via existing `set_pose` IPC. Runtime stops owning `mc->resize`, `mc->title_drag`, `mc->title_rmb_drag`. Probably several PRs.
- **Cursor sprite render policy → controller.** Either (a) controller submits cursor as an additional chrome layer the runtime composites, or (b) runtime exposes "render cursor at this pose with this shape" via an extension API and the controller drives it. (a) reuses existing chrome plumbing and is preferred.
- **Once the policy migrates**, revisit whether the public `xrWorkspaceHitTestEXT` API is still useful or if all controller raycast can be controller-local.

### Constraints we are NOT relaxing

- The runtime continues to do per-eye parallax tile rendering using DP-cached eye positions (the DP listener pattern landed in #251). That is unambiguously runtime work — it's composition, not interaction.
- The DP continues to own SR SDK eye-tracking state internally. Per #251.

## Related

- ADR-016 — workspace controllers own their tray surface and lifecycle.
- ADR-014 — shell owns rendering mode.
- `docs/architecture/separation-of-concerns.md` — high-level layer ownership.
- Memory `[Controllers own motion]` — drove the original framing.
- Memory `[Compositor eye-pos layering]` — sibling concern, resolved by #251.
