# Shell Phase 5: Agent Prompt — Spatial App Launcher

Use this prompt to start a new Claude Code session for implementing Phase 5 on branch `feature/shell-phase5`.

---

## Prompt

```
I'm working on the DisplayXR shell — a spatial window manager for 3D lenticular displays. We're implementing Phase 5: the Spatial App Launcher. This adds a proper 3D UI panel for browsing and launching DisplayXR apps, replacing the Phase 4C minimal Ctrl+L stub (which just launches the first registered app).

## Context

Read these docs first (in order):
1. `CLAUDE.md` — project overview, build commands, architecture
2. `docs/roadmap/shell-phase5-plan.md` — **the plan you're implementing** (exploration phase + launcher UI)
3. `docs/roadmap/shell-phase5-status.md` — task checklist (update as you complete tasks)
4. `docs/roadmap/shell-phase4c-status.md` — what Phase 4C delivered (the foundation you build on)
5. `docs/roadmap/shell-runtime-contract.md` — IPC protocol boundary (runtime vs shell)
6. `docs/reference/window-drag-rendering.md` — how the multi-compositor handles windows (useful for launcher panel rendering)
7. `docs/roadmap/3d-shell.md` and `docs/roadmap/spatial-desktop-prd.md` — product vision for the shell

## Branch

You are on `feature/shell-phase5`, branched from `main` after Phase 4C was merged (PR #138). All work goes here. Commits should reference #43 (Spatial OS tracking issue) or add #44 if shell-specific.

## What Phase 5 needs

Phase 5 has TWO parts. Start with Part 1 (discovery exploration) — do NOT jump straight to UI work.

### Part 1: App Discovery — start with exploration

Phase 4C shipped `registered_apps.json` at `%LOCALAPPDATA%\DisplayXR\registered_apps.json` with manually-listed apps. Phase 5 needs to **auto-discover installed DisplayXR apps** so users don't have to hand-edit JSON.

**Your first task is an exploration phase.** Before writing any code:

1. Read `docs/roadmap/shell-phase5-plan.md` Part 1 thoroughly.
2. Research the five exploration questions listed there:
   - Can we detect OpenXR apps by scanning PE imports?
   - Is a `.displayxr.json` sidecar convention viable?
   - What install locations should we scan?
   - How do other XR ecosystems (SteamVR, Oculus, WMR) handle this?
   - What can the runtime tell us about known apps?
3. Look at the existing Phase 4C launcher code in `src/xrt/targets/shell/main.c` — specifically the `registered_app` struct, `registered_apps_load/save`, `shell_launch_registered_app`, and the Ctrl+L handler. Understand what's already there.
4. Look at existing demo/test app layouts: `test_apps/cube_handle_*_win/`, `demos/*/`.
5. Write a ~500-word report at `docs/roadmap/shell-phase5-discovery-findings.md` with:
   - Summary of findings for each exploration question
   - A concrete **recommendation** for the discovery strategy (probably a hybrid)
   - A proposed `.displayxr.json` spec if that's part of the recommendation
   - Open questions that need the user's input before implementation

**Stop after writing the exploration report. Ask the user to review it before implementing anything.** The user explicitly wants the exploration to happen first.

### Part 2: Spatial Launcher UI

Only start Part 2 after exploration is approved.

The launcher is a spatial window rendered inside the multi-compositor shell window. It shows:
- **Running section**: apps currently connected as IPC clients (highlight tiles)
- **Installed section**: apps from the scanned registry + `registered_apps.json`
- **Browse for app**: file dialog to add an arbitrary exe

**Rendering approach:** Extend the existing multi-compositor's drawing code (title bars, buttons, bitmap font atlas in `d3d11_bitmap_font.h`) to draw a panel with a grid of tiles. Start with the simplest possible layout — grid of colored quads with text labels — then iterate.

**Interaction:**
- `Ctrl+L` opens the launcher (replacing current Phase 4C behavior of launching first app)
- `Esc` closes it
- Mouse hover + click to launch
- Arrow keys + Enter for keyboard nav
- Right-click tile for context menu (remove)

**Launch path:** Reuse the existing `shell_launch_registered_app()` from Phase 4C. Don't duplicate the env-var-setting CreateProcess logic.

**Running detection:** Already available via `ipc_call_system_get_clients` + `ipc_call_system_get_client_info` (used in Phase 4C's polling loop). The launcher queries this when it opens and highlights matching tiles.

## What Phase 5 is NOT

- Not a Steam-style app store
- Not cloud app sync
- Not arbitrary icon extraction from exe resources (sidecar + fallback for v1)
- Not multi-display launcher positioning
- Not localization

Keep the scope tight. Ship a working grid with discovery in a reasonable number of commits.

## Key files to reference

- `src/xrt/targets/shell/main.c` — existing launcher code (`registered_app`, `shell_launch_registered_app`, Ctrl+L handler)
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — multi-compositor rendering; find `render_title_bar_*` and the blit shader usage for an example of drawing UI in the shell window
- `src/xrt/compositor/d3d11_service/d3d11_bitmap_font.h` and the glyph atlas setup — for rendering tile labels
- `src/xrt/compositor/d3d11_service/d3d11_service_shaders.h` — existing `BlitConstants` struct (already supports rounded corners, glow, etc.)

## Commit style

- Commit per task (or small group of related tasks) — not one big blob
- Reference #43 (or #44 if shell-specific) in every commit
- Use `Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>` in the commit footer

## Testing

Build locally: `scripts\build_windows.bat build`. Launch the shell with a cube app for running-app testing:
```
_package\bin\displayxr-shell.exe test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
```

Press Ctrl+Space to activate shell, then Ctrl+L to open the launcher.

## When to ask the user

- After the exploration report is written — before implementing the discovery scanner
- Before making UI layout decisions that could be "bikeshed" — provide options and ask
- When you hit any deadlock, cross-process issue, or scope question — Phase 4C had many of these, don't power through without asking

## Deliverables

- `docs/roadmap/shell-phase5-discovery-findings.md` — the exploration report
- Working discovery scanner + populated `registered_apps.json` on first launch
- Working spatial launcher panel (Ctrl+L) with grid, running indicator, launch dispatch
- Updated `shell-phase5-status.md` with tasks checked off
- Clean commits on `feature/shell-phase5`
```

---

## Notes for the user running the agent

- Phase 5 explicitly starts with an exploration phase. Don't approve a jump straight to implementation — the agent should produce `shell-phase5-discovery-findings.md` first.
- The "automatic app discovery" problem is not fully solved by any XR ecosystem; a hybrid approach is expected (sidecar + scan + manual registry merge).
- The launcher UI should reuse Phase 4A-4C infrastructure (multi-compositor drawing, bitmap font atlas, title bar rendering, slot-based rendering) — resist temptation to add a new windowing layer.
- Test with the existing `test_apps/cube_handle_*_win` apps — they're the most diverse set for validating discovery.
