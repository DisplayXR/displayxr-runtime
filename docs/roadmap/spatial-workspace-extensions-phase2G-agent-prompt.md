# Phase 2.G Agent Prompt — Layout Presets + ESC/Empty-Workspace Cleanup

Self-contained prompt for a fresh agent session implementing Phase 2.G of the workspace-extensions effort. Drop into a new session as the user message after `/clear`. The agent has no memory of the prior design conversations — this prompt assumes nothing.

---

## What Phase 2.G is for

You're picking up Phase 2.G — the **layout-preset migration** + **ESC carve-out cleanup**. Phase 2.D shipped the input-routing surface (v3 → v4); Phase 2.I migrated the first-party DisplayXR Shell off internal IPC onto the public OpenXR extensions; Phase 2.I-followup turned the runtime controller-agnostic. The runtime now exposes:

- `XR_EXT_spatial_workspace v5` — lifecycle, capture clients, window pose + visibility, hit-test (with region), focus, input-event drain, pointer capture, frame capture, client enumeration
- `XR_EXT_app_launcher v2` — launcher tile registry with 3D-icon support

The first-party shell has fully migrated. **Phase 2.G's job:** finish the policy/mechanism split for layout presets and ESC handling.

Today the runtime owns:
- The `Ctrl+1` / `Ctrl+2` / `Ctrl+3` layout-preset hotkeys (handled in `comp_d3d11_service.cpp`'s WndProc-poll path).
- The `apply_layout(grid|immersive|carousel)` implementation that recomputes every active client's pose.
- The MCP `apply_layout_preset` tool that lets AI agents trigger named layouts via `comp_d3d11_service_apply_layout_preset`.
- The "empty workspace" state machine (workspace mode ON, no clients yet) plus the ESC carve-out that swallows ESC when in this state so the user doesn't accidentally close the empty workspace window.

After 2.G the runtime owns: the pose-set primitive (already shipping as `xrSetWorkspaceClientWindowPoseEXT`). The controller owns: which named presets exist, what each one means, and Ctrl+1..4 dispatch.

## Read these in order before touching code

1. `docs/roadmap/spatial-workspace-extensions-plan.md` — three-phase master plan; finds where 2.G sits.
2. `docs/roadmap/spatial-workspace-extensions-phase2D-agent-prompt.md` — the 2.D + 2.I prompts. Skim the established six-commit shape and the `XR_EXT_spatial_workspace` v3→v4→v5 evolution.
3. `docs/roadmap/spatial-workspace-extensions-followups.md` — outstanding items from 2.D + 2.I. Two of them touch 2.G's territory (pointer-capture enforcement, ESC reentry state machine) — decide whether to fold into this phase or defer.
4. **Code reads (~30 minutes):**
   - `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` lines ~5470–5600 (`apply_layout`) and ~5970–5990 (Ctrl+1/2/3 hotkey dispatch).
   - Same file lines ~12350–12400 (`comp_d3d11_service_apply_layout_preset` MCP wrapper).
   - `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` lines ~593–610 (ESC handling + workspace_mode_active gate).
   - `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` lines ~12080–12100 (`ensure_workspace_window` + the empty-workspace render-timer comment).
   - `src/xrt/targets/shell/main.c` — search for `Ctrl+1` / `apply_layout` / `preset` to see what (if anything) the shell knows about presets today (likely nothing — the runtime intercepts the hotkeys).

## Branch + prerequisites

- Start from `main` after Phase 2.D + 2.I + 2.I-followup have merged.
- New branch: `feature/workspace-extensions-2G`.
- Confirm `main` includes the prior phases: `git log --oneline main | grep -E "Phase 2\.D|Phase 2\.I"` should return ~21 hits.

## The architectural choice (decide first)

The master plan says "layout-preset semantics move to the controller." Three concrete shapes for that:

| Option | What | Cost | Recommended? |
|---|---|---|---|
| **A. Pure controller** | Runtime knows nothing about presets. Controller computes per-window poses and calls `xrSetWorkspaceClientWindowPoseEXT` per window. Ctrl+1..4 keys forwarded via `xrEnumerateWorkspaceInputEventsEXT` like any other key. | Smallest runtime diff; controller owns 100% of policy. MCP `apply_layout_preset` tool moves out of the runtime entirely (becomes a shell-side tool or ships with the controller). | **Recommended** if MCP can move with the shell. |
| **B. Named-preset enumerate** | Add small public extension surface: `xrEnumerateWorkspaceLayoutPresetsEXT(session, capacity, *count, names)` and `xrApplyWorkspaceLayoutPresetEXT(session, name)`. Runtime keeps `apply_layout` so MCP/external tools can trigger by name; presets are still defined controller-side and registered via a new `xrRegisterWorkspaceLayoutPresetEXT` call. | Larger surface; gives MCP a stable entry point. Controller still owns the math; runtime stores name → opaque controller-side dispatch. | If MCP must stay in the runtime DLL. |
| **C. Hybrid status quo** | Leave `apply_layout` in the runtime for now; only migrate Ctrl+1..4 hotkey dispatch (controller intercepts via input-event drain). | Minimal change; defers the real split. | Only if 2.C (chrome rendering) needs to land first. |

The architectural-decision section is for **the agent picking this up to resolve with the user**. Default to A unless the user pushes back; B is the fallback.

## What ships in Phase 2.G (assuming Option A)

### Removed from the runtime

- `comp_d3d11_service.cpp::apply_layout` — delete.
- The Ctrl+1/2/3 hotkey-dispatch block in `comp_d3d11_service.cpp` (~lines 5970–5990) — delete.
- `comp_d3d11_service_apply_layout_preset` C wrapper (~line 12363) — delete.
- The shell's `// Ctrl+1-4: layout presets (handled server-side) → don't forward` gate in `comp_d3d11_window.cpp` (~line 597) — delete; Ctrl+1..4 now flow through normally to either the focused HWND (which usually ignores them) or the public input-event drain.

### Removed from the IPC + MCP surface

- The MCP tool `apply_layout_preset` in `src/xrt/ipc/server/ipc_mcp_tools.c` — delete or reroute. If the user wants MCP-driven layouts to keep working, the MCP tool moves to the workspace controller (e.g. `displayxr-shell` exposes its own MCP endpoint) — that's outside the runtime now.

### Added to the controller (`src/xrt/targets/shell/main.c` — but the shell has decoupled from this repo, so this is **optional** in 2.G)

- Ctrl+1..4 hotkey registration (already partially exists — extend to call into a per-preset pose-computer that walks the client list via `xrEnumerateWorkspaceClientsEXT` + `xrGetWorkspaceClientWindowPoseEXT` and pushes new poses via `xrSetWorkspaceClientWindowPoseEXT`).
- Carry the grid/immersive/carousel formulas over from the runtime as pure controller-side math.
- **NOTE:** if the shell has already extracted to `DisplayXR/displayxr-shell`, do this work there instead. Check `targets/CMakeLists.txt` for whether `add_subdirectory(shell)` still exists — if not, the shell moved.

### ESC carve-out simplification

With layout-preset state out of the runtime, the workspace-mode reentry state machine simplifies:
- `workspace_mode_active` flag stays (still gates ESC swallowing).
- `ensure_workspace_window` (the empty-workspace bring-up) can simplify because the runtime no longer needs to render the "Press Ctrl+L for launcher" hint state.
- ESC behavior should become: ESC always passes through to the focused HWND (which usually means "close the focused app" if the app handles it). Only when the workspace is in pure-empty mode does ESC close the workspace window — and that special case can move to the controller too (controller polls `xrEnumerateWorkspaceClientsEXT`, when count == 0 binds ESC to a deactivate call).

The cleanest end-state: runtime knows nothing about ESC at all. Controller decides.

## Recommended commit sequence (assuming Option A)

Six commits. Same shape as 2.A → 2.D.

### Commit 1 — Migrate Ctrl+1..4 hotkey ownership

- Stop runtime from intercepting Ctrl+1..4. Remove the gate in `comp_d3d11_window.cpp` and the dispatch in `comp_d3d11_service.cpp`.
- Extend `is_workspace_reserved_key` accordingly (Ctrl+1..4 are no longer reserved).
- Verify Ctrl+1..4 keys appear in `xrEnumerateWorkspaceInputEventsEXT` drain (they're regular KEY events now).
- **Acceptance:** Ctrl+1..4 in the workspace produces KEY events on the public drain; runtime no longer applies any layout.

### Commit 2 — Move layout math to the controller (or document follow-up if shell extracted)

- If shell still in this repo: port `apply_layout`'s grid/immersive/carousel math from `comp_d3d11_service.cpp` into the shell. Wire Ctrl+1..4 in the shell's WndProc / input-drain handler.
- If shell extracted: skip; file an issue on `DisplayXR/displayxr-shell` repo.
- **Acceptance:** Pressing Ctrl+1..4 in the workspace re-arranges windows the same way it did before (visual regression test).

### Commit 3 — Delete runtime-side `apply_layout` + MCP tool

- Remove `apply_layout`, `comp_d3d11_service_apply_layout_preset`, and the MCP `apply_layout_preset` tool.
- Remove the `mc->current_layout` field if no longer read (TAB Z-reorder for "Stack" preset may still use it — check).
- **Acceptance:** Build green; standalone smoke test still passes; nothing in the runtime references "layout preset" anymore.

### Commit 4 — ESC simplification

- Remove the runtime's "ESC swallowed when workspace_mode_active and no app maximized" carve-out (or document why it must stay).
- Simplify `ensure_workspace_window` — the empty-workspace render-timer comment becomes inaccurate once the controller owns this state; remove the timer or scope it down.
- **Acceptance:** ESC behavior verified manually: in workspace mode with apps, ESC goes to focused app; in empty workspace, controller closes (via deactivate) or runtime falls through to default WM_CLOSE handling.

### Commit 5 — `XR_EXT_spatial_workspace` v5 → v6 doc/spec bump (if surface changed)

- If Option A is taken cleanly, no public surface changes — skip this commit.
- If Option B was needed, this is where the new PFNs land (header + state-tracker + IPC bridge + dispatch).

### Commit 6 — Test app + cleanup

- Extend `test_apps/workspace_minimal_d3d11_win` to demonstrate controller-side preset application (e.g. on the orchestrator success path, push two capture clients then call `xrSetWorkspaceClientWindowPoseEXT` for each to a "grid" layout).
- Service-side cleanup: any `apply_layout`/`current_layout` mentions in logs or comments removed.

## Acceptance criteria for the whole phase

- ✅ Runtime contains zero references to "layout preset" / `apply_layout` / `current_layout` (modulo the controller-owned `mc->current_layout` field if TAB Z-reorder needs it).
- ✅ Ctrl+1..4 keys flow through the public input-event drain.
- ✅ ESC carve-out simplified or removed.
- ✅ MCP `apply_layout_preset` tool removed from the runtime (or rerouted to the controller per Option B).
- ✅ Visual regression: with the first-party controller (or whatever controller the user is testing), Ctrl+1..4 produce the same window arrangements they did before.
- ✅ Standalone test app smoke still PASSes.
- ✅ Windows MSVC CI green.

## Hand-off notes

- **The shell may have extracted by the time you read this.** Check `git log --oneline main | grep -E "shell.*extract|2\.J"`. If 2.J shipped, the controller-side work in Commit 2 happens in the `displayxr-shell` repo, not here. The runtime-side commits (1, 3, 4) are unaffected.
- **`comp_d3d11_service_apply_layout_preset` may have callers besides MCP.** Grep before deleting. If anything else in the runtime calls it, that caller likely also needs to migrate.
- **The "empty workspace" state machine is real.** Read `comp_d3d11_service.cpp::ensure_workspace_window` carefully before deleting the empty-workspace render-timer. The runtime needs SOME window to exist for the workspace to be addressable (e.g. for SetForegroundWindow targeting); removing it entirely breaks Ctrl+Space toggle.
- **Outstanding pointer-capture enforcement** (from `followups.md` item #1) is independent of 2.G but the WndProc click-filtering path you'll touch in Commit 1 is the same path that needs the pointer-capture fix. Consider folding it in.
- Per `feedback_test_before_ci.md`: build + smoke locally before pushing. Don't run `/ci-monitor` yourself — that's user-triggered.
- Per `feedback_dll_version_mismatch.md`: push runtime binaries to `Program Files\DisplayXR\Runtime` after rebuild so smoke tests pick them up under elevation.

## What unblocks once Phase 2.G passes

- **Phase 2.J — shell repo extraction**. The shell still has runtime-internal IPC code paths it could lean on; once 2.G is in, the shell is a strict consumer of the public extension surface and can fork cleanly. (If 2.J already shipped, this is moot.)
- **Phase 2.C — chrome rendering**. The heaviest remaining migration. Chrome (close/min/max buttons, app name, title bar) moves to controller-rendered overlays composited by the runtime. Allocate a full sub-session.
- **macOS port**. Mirror the public surface with a Cocoa-native event source equivalent of the Win32 WndProc. Headers stay platform-neutral; only the implementation differs.
