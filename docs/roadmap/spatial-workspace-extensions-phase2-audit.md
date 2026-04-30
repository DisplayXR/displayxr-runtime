# Phase 2 Audit — `comp_d3d11_service.cpp` mechanism vs policy

**Status:** Draft, 2026-04-28. Companion to [spatial-workspace-extensions-plan.md](spatial-workspace-extensions-plan.md) and [spatial-workspace-extensions-headers-draft.md](spatial-workspace-extensions-headers-draft.md).

After Phase 1, `comp_d3d11_service.cpp` (12,212 lines) still holds 162 `shell`/`Shell` mentions. Each one belongs to one of three categories:

- **Mechanism** — composition / lifecycle / rendering machinery the runtime owns. Survives Phase 2 under a neutral name (`workspace_*`).
- **Policy** — what to display, when, with what UX. Moves to the workspace controller process behind the spatial-workspace + app-launcher extensions.
- **Gone** — exists only because the runtime tries to handle workspace concerns; deleted outright when policy migrates.

This doc walks the file cluster by cluster, labels each, and proposes a migration order. The next implementer picking up Phase 2 reads this and starts migrating without re-reading the whole file.

## Cluster summary

| Cluster | Lines | Classification | Migration step |
|---|---|---|---|
| Lifecycle state-machine fields (`pending_shell_reentry`, `last_shell_render_ns`) | 258-259, 500-502 | Mechanism — rename | Phase 2 final cleanup |
| Launcher app registry (state + comments) | 327-367 | **Policy — moves out** | Phase 2.B (launcher) |
| Suspend-deactivated lifecycle state | 742-747 | Mechanism — rename | Phase 2 final cleanup |
| Cursor + capture-thread state comments | 761, 866, 880 | Mechanism — rename | Phase 2 final cleanup |
| `service_set_workspace_mode` mirror + comp_d3d11_window flag set | 916-923, 4315-4317, 4586 | Mechanism — rename (already partial in Phase 1) | Phase 2.E (D3D11 window deferred set) |
| Per-mode resource creation (atlas-only, DP creation skip) | 1983-2049, 2180-2184, 2532 | Mechanism — rename | Phase 2 final cleanup |
| `shell_input_event` struct + ring buffer | 3558, 3587 | Mechanism — rename to `workspace_input_event` | Phase 2.D (input routing) |
| Multi-comp first-layer-commit init | 4271-4278 | Mechanism — rename | Phase 2 final cleanup |
| `comp_d3d11_window_set_shell_dp` call | 4558 | Mechanism — rename when callee renames | Phase 2.E |
| **Spatial raycast hit-test** (`shell_hit_result`, `shell_raycast_hit_test`) + callers | 4660-4904, 6003, 6046, 6154, 6497 | Mechanism — rename + promote to `xrWorkspaceHitTestEXT` | Phase 2.F (hit-test promotion) |
| **Title-bar height math** (`has_shell_title_bar`) | 5010-5037, 7473 | **Policy — gone** when chrome migrates | Phase 2.C (chrome) |
| `compositor_layer_commit` workspace-mode branches | 5616-5732 | Mechanism — rename; some branches simplify when policy migrates | Phase 2 final cleanup |
| File-dialog foreground-permission grants | 5789, 6140 | Mechanism — rename | Phase 2 final cleanup |
| Scroll-wheel input policy comment | 6553 | Mechanism — rename | Phase 2 final cleanup |
| Atlas / SRGB shader-blit comments | 6987, 7058-7173, 9569-9658 | Mechanism — rename | Phase 2 final cleanup |
| Launcher-rendering tile-grid lifecycle | 8198, 8200, 8484 | **Policy — gone** when launcher migrates | Phase 2.B |
| File-trigger screenshot path (`shell_screenshot_*`) | 8831-8840 | Mechanism — rename to `workspace_screenshot_*` | Phase 2 final cleanup |
| Atlas-clear / DP ownership comments | 9090, 9169 | Mechanism — rename | Phase 2 final cleanup |
| Bridge-override stage-3 comment | 9305-9313 | Mechanism — rename | Phase 2 final cleanup |
| Reverse-hot-switch lifecycle (`pending_shell_reentry`) | 9860-9905, 10126-10128, 11777 | Mechanism — rename | Phase 2 final cleanup |
| `compositor_create_native` workspace-mode activation | 10238-10285 | Mechanism — rename | Phase 2 final cleanup |
| DP/window ownership comments | 10756-11025, 11560-11569 | Mechanism — rename | Phase 2 final cleanup |
| `set_client_window_pose` / `set_visibility` log strings | 11413, 11441 | Mechanism — rename log prefix | Phase 2 final cleanup |
| `add_capture_client` log strings | 11620-11641 | Mechanism — rename log prefix | Phase 2.A (capture) |
| `ensure_workspace_window` lifecycle log strings | 11751-11849 | Mechanism — rename log prefix; some lifecycle simplifies | Phase 2 final cleanup |
| `deactivate_workspace` lifecycle log strings | 11872-11960 | Mechanism — rename log prefix | Phase 2 final cleanup |
| **Launcher functions** (`add/clear/poll/show/running_tile_mask`) + ~~`apply_layout_preset`~~ | 11960-12118 | **Policy — moves out** (and most code deletes) | Phase 2.B + ~~Phase 2.G~~ ✅ apply_layout_preset shipped |

## Counts by category

| Category | Approximate line count | What happens |
|---|---|---|
| Mechanism (stays + gets renamed) | ~115 mentions across ~25 clusters | Renamed in a final cleanup pass after policy migrations land. Most are doc comments and log-string prefixes; the actual code keeps doing the same thing. |
| Policy (moves to workspace process) | ~30 mentions across 4 clusters (launcher registry+rendering, chrome title-bar math, layout-preset semantics) | Function bodies + state move to the workspace controller. The runtime exposes neutral primitives via `XR_EXT_spatial_workspace` / `XR_EXT_app_launcher`. |
| Gone (delete when policy moves) | ~17 mentions in chrome + launcher rendering | Deleted as a side effect of policy migration — no equivalent in the workspace process, no equivalent in the runtime. |

After Phase 2: the file shrinks meaningfully. The launcher cluster (4660 lines worth of state + UI) and the chrome math (~30 lines of title-bar geometry) leave the runtime entirely; everything else gets renamed in place.

## Migration order — Phase 2 sub-steps

The plan doc proposed an order; the audit confirms it. Phase 2 is preceded by **Phase 2.0** (architectural prep — neither sub-step touches `comp_d3d11_service.cpp` directly):

### Phase 2.0 — Architectural prep (gates Phase 2.A onward)

**Two sister docs, lands as one or two short branches:**

- **[Workspace controller detection](spatial-workspace-controller-detection.md)** — orchestrator file-presence check + sidecar `.controller.json` manifest read; conditional tray UI; hotkey gated on availability. Makes the runtime a real platform (installable without the shell, with a useful WebXR + standalone-OpenXR experience).
- **[Workspace activation auth handshake](spatial-workspace-auth-handshake.md)** — orchestrator-PID match replaces the literal `"displayxr-shell"` `application_name` match in `ipc_server_handler.c:296`. Removes the last brand coupling in the runtime.

Lands as a few hundred lines of orchestrator + tray + IPC-handler changes. Doesn't touch the compositor body — the 162 mentions in this audit are untouched by Phase 2.0. Phase 2.A starts the compositor migration.

Below: the original 8 sub-steps, lowest-blast-radius first.

### Phase 2.A — Capture client lifecycle

**Touches:** 11620-11641, plus the public `xrAddWorkspaceCaptureClientEXT` / `xrRemoveWorkspaceCaptureClientEXT` extension surface.

**Why first:** Already nearly neutral — the bulk of the work is renaming log prefixes and wiring the existing `comp_d3d11_service_add_capture_client` to its extension entry point. Lowest blast radius. Validates the basic extension dispatch plumbing before any policy migration.

**Risk:** Low. No logic changes.

### Phase 2.B — Launcher tile registry + click polling + running-tile mask

**Touches:** 327-367 (state), 8198-8484 (rendering), 11960-12118 (functions). Plus the workspace-controller side: the controller starts owning the registry array and pushing via `xrAddLauncherAppEXT`.

**Why second:** The launcher is well-bounded. Only the workspace controller pushes to it; only the runtime renders it; the surface between them is small. Validates the `XR_EXT_app_launcher` extension end-to-end. Most of the runtime-side state in the field deletes; runtime keeps only the rendered tile-grid overlay (mechanism: how to draw N tiles), not the registry (policy: which N tiles).

**Risk:** Medium. The "empty workspace shows registered apps before any client connects" UX needs careful handling — runtime can render the tile grid while the workspace controller is connected even with zero app clients, but the controller can't push tiles before it itself connects, so there's a chicken-and-egg moment to handle. Open question: should the runtime cache the last-pushed registry across deactivate/activate cycles, or does the controller re-push on every activate? Recommend the latter (simpler runtime, controller already does this).

### Phase 2.C — Title-bar chrome rendering

**Touches:** 5010-5037, 7473, plus the workspace-controller side (controller draws chrome into its own client window which the runtime composites).

**Why third:** Most invasive. Today the runtime draws title bars over the app's atlas as a separate composite pass. Moving chrome to a workspace-rendered layer requires:
- The workspace controller renders chrome (close button, app name) into one of its own swapchain images.
- The runtime composites that swapchain alongside the app's, at a slightly higher Z (or as an overlay layer).
- The runtime stops drawing title bars at all — the chrome-rendering code path deletes.

**Risk:** High. Affects the composite pipeline and the input pipeline (title-bar drag is a common case). Allocate a full sub-session.

### Phase 2.D — Input routing (focus + raw event forwarding)

**Touches:** 3558, 3587 (`shell_input_event`), plus all input-handling paths. Plus the new `xrSetWorkspaceFocusedClientEXT` extension function.

**Why fourth:** Largely independent design work. Today the runtime forwards mouse/keyboard from the workspace's window to the focused client's HWND, with the runtime making the focus call. Phase 2 has the workspace controller decide focus and the runtime forward raw events to whoever the controller designated. The extension surface for this isn't yet in the header sketch — needs design first.

**Risk:** High. Input is fragile; regressions are user-visible.

### Phase 2.E — D3D11 window deferred functions

**Touches:** 4315-4317, 4558, 4586, 916-923, plus `comp_d3d11_window.{h,cpp}` (the deferred-Phase-1 set: `shell_mode_active` field, `set_shell_mode_active`, `set_shell_dp`, `shell_input_event` struct, `SHELL_INPUT_RING_SIZE` constant).

**Why fifth:** These are internal D3D11 window concerns the runtime keeps. Renaming them to `workspace_*` is a mechanical pass once Phase 2.D's input routing is settled — the `shell_input_event` struct rename has to land alongside the input-routing code that passes it around.

**Risk:** Low. Pure mechanical rename if input routing is stable.

### Phase 2.F — Hit-test extension promotion

**Touches:** 4660-4904 (function definition), 6003, 6046, 6154, 6497 (callers). Plus `xrWorkspaceHitTestEXT` entry point in the extension dispatch.

**Why sixth:** The function and its callers stay in the runtime — they're mechanism. The work is exposing `shell_raycast_hit_test()` as a public extension function so the workspace controller can call it directly instead of duplicating the geometry. Independent of the chrome migration; could move earlier if needed.

**Risk:** Low. Net-add — the existing callers keep using the function internally; the extension entry point is a thin wrapper.

### Phase 2.G — Layout presets + ESC/empty-workspace cleanup ✅ shipped

**Touched:** `apply_layout`, `enter_dynamic_layout`, `dynamic_layout_tick`, `carousel_compute_pose`, `compute_zmax`, the Ctrl+1..3 hotkey dispatch, the carousel LMB/scroll/TAB UI handlers, `mc->current_layout`, `mc->dynamic_layout`, `mc->regrid_pending_ns`, the debounced re-grid, and `comp_d3d11_service_apply_layout_preset` + its MCP tool. ~733 net deletions in `comp_d3d11_service.cpp` + `ipc_mcp_tools.c`. Pointer-capture follow-up #1 folded into Commit 1.

**Decisions made during the migration:**
- Layout-preset semantics moved to the controller (Option A — pure controller). The shell now ports the grid + immersive + carousel-snap math.
- Carousel ships as a static snap on the controller side (no per-frame tick); a follow-up can add animation if the regression is felt.
- ESC carve-out kept as a safety net (annotated in `comp_d3d11_window.cpp`). The empty-workspace launcher hint and reentry state machine stay — they're rendering hygiene, not policy.
- `compute_grid_layout` stays in the runtime; auto-placement on client connect still uses it. Deferred as a separate slice of policy.
- Three MCP tests removed (`tests/mcp/test_focus_preset.{sh,bat}` + `_focus_preset_helper.py`) — the tool they exercised is gone.

**Risk realized:** Lower than predicted. The ESC / dismiss / restore flow needed only a comment annotation; the workspace state machine simplification was deferred to Phase 2.C (chrome rendering migration) where it'll get a proper redesign.

### Phase 2.H (final cleanup pass)

**Touches:** All remaining mechanism mentions (~115 across 25+ clusters). Pure rename pass: `shell` → `workspace` in comments and log prefixes; `pending_shell_reentry` → `pending_workspace_reentry`; `last_shell_render_ns` → `last_workspace_render_ns`; `shell_input_event` → `workspace_input_event`; `shell_screenshot_*` filenames → `workspace_screenshot_*`; etc.

**Risk:** Trivial — same perl-rename pattern Phase 1 used, with the lessons-learned-the-hard-way from `feedback_perl_rename_gotchas.md` (drop left `\b`, inline file lists).

## Specific issues called out for Phase 2 sessions

These are gotchas that won't be obvious until you start the migration. Capturing them here so the next session doesn't re-discover them.

### Launcher's chicken-and-egg startup

The runtime today renders the launcher tile grid *before* any app client connects. This is possible because the workspace controller has already pushed its registry via IPC. Phase 2.B preserves this by having the controller push on every `xrActivateSpatialWorkspaceEXT`, before any client work happens. Don't try to "lazy-load" tiles — keeps the empty-workspace UX broken.

### `pending_shell_reentry` is two lifecycle features in one field

Reading the code at 9866 and 11777 it looks like one flag, but it actually serves two transitions:
1. Standalone → workspace re-activate (a previously-direct-rendering app gets re-claimed by a re-activated workspace).
2. Workspace dismissed → workspace re-activated (after Ctrl+Space toggles the workspace off and back on).

When the lifecycle simplifies in Phase 2.G, separate these into two flags or two state-machine arms — folding them together saved code in the original spatial-shell-phase4D work but obscures the transitions for anyone refactoring.

### Title-bar height affects the hit-test, not just the renderer

Lines 5010-5037 wire `has_shell_title_bar` into the hit-test geometry — they expand the window's effective "ext_top" by the title-bar height so a click in the title-bar area registers. When chrome migrates to a workspace-rendered layer (Phase 2.C), the hit-test geometry needs to know about that layer too — *or* the workspace-rendered chrome is its own workspace client (with its own pose), so the hit-test naturally handles it. Recommend the latter: the workspace controller renders chrome into a per-app companion swapchain, positions it just above the app via `xrSetWorkspaceClientWindowPoseEXT`, and the existing hit-test machinery treats it as a regular client.

### File-trigger screenshot is a dev-tool fallback

The `shell_screenshot_trigger` file-poll mechanism at 8831-8840 was added for ad-hoc debugging when the IPC path wasn't available. The `xrCaptureWorkspaceFrameEXT` extension function will be the proper path. The file-trigger fallback can stay (with renamed paths) for emergency debug; not worth deleting.

## Estimated post-Phase-2 file size

Rough math:
- Today: 12,212 lines
- Launcher state + functions deleted: ~150 lines
- Chrome / title-bar math deleted: ~40 lines
- Layout-preset semantics deleted: ~80 lines
- Lifecycle simplification (ESC carve-out, reentry state): ~30 lines
- ~300 lines of saved code

That's ~2.5% smaller. Most of the value is *not* size reduction — it's that the runtime's behavior is now explained by extension semantics, not by reading the file. After Phase 2, anyone wanting to understand "what happens when an app connects in workspace mode" reads `XR_EXT_spatial_workspace.h`, not 12K lines of compositor code.

## What this audit doesn't cover

- **`comp_d3d11_window.{h,cpp}`** — has its own deferred set (`shell_mode_active`, `set_shell_mode_active`, `set_shell_dp`, `shell_input_event` struct, `SHELL_INPUT_RING_SIZE`). Phase 2.D + 2.E touch these; they're 5-ish symbols total, not enough to need their own audit.
- **`ipc_server_handler.c:296`** — the literal `"displayxr-shell"` application_name match. Resolved by [spatial-workspace-auth-handshake.md](spatial-workspace-auth-handshake.md) (orchestrator-PID match). Not part of this file.
- **WebXR bridge references** — `webxr_bridge/main.cpp` has its own "shell mode" / "shell-window" / "WebXR-in-shell" terminology tied to the named WebXR-in-shell feature. Coordinated rename belongs with the WebXR bridge's own lifecycle, not Phase 2 of this effort. Noted as deferred in the second-pass cleanup commit (`d4ebf84bd`).
