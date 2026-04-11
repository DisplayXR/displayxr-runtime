# Agent prompt: WebXR Bridge v2 Phase 2 (#139)

Use this prompt to start a fresh Claude Code session on a Windows machine with a Leia SR display attached, to implement Phase 2 of WebXR Bridge v2 on branch `feature/webxr-bridge-v2`.

---

## Prompt

```
I'm implementing Phase 2 of WebXR Bridge v2 for DisplayXR (#139). Phase 1 already shipped — the native bridge host exists at `src/xrt/targets/webxr_bridge/main.cpp` and receives `RENDERING_MODE_CHANGED_EXT` events from the D3D11 service compositor through a just-landed fix (#142). Phase 2 adds the browser-facing half: a loopback WebSocket server inside the bridge, a Chrome MV3 extension that wraps `navigator.xr` with a `session.displayXR` surface, and a three.js sample. **No runtime changes** unless a specific framebuffer-clamp check fails (step 1 below).

The full phase 2 plan is at `docs/roadmap/webxr-bridge-v2-phase2-plan.md`. Read it first — it is authoritative.

## Current state of the branch

You are on `feature/webxr-bridge-v2`, which is ahead of `main`. Relevant recent commits:

- `5c411cfdb` — Fix RENDERING_MODE_CHANGED fan-out (#142) (#139). **Verified end-to-end**: with the bridge running headless and Chrome WebXR driving frames, V-key toggles produce `RENDERING_MODE_CHANGED` + `HARDWARE_DISPLAY_STATE_CHANGED_EXT` events in the bridge terminal on every press. This unblocked Phase 2.
- `f9ddb3117` — Docs: fix plan + agent prompt for RENDERING_MODE_CHANGED fan-out (#142)
- `7c5f60ec3` — Scaffold WebXR Bridge v2 host (Phase 1) (#139). Bridge lives at `src/xrt/targets/webxr_bridge/`, uses `XR_MND_headless` (no D3D11 binding fallback needed), logs display info + events, installs to `_package\bin\displayxr-webxr-bridge.exe`.
- `d4347ba67` — WebXR Bridge v2 plan, agent prompt, scrap old bridge (#139)

`webxr-bridge/` at repo root does NOT currently exist — Phase 1's Phase 0 deletion removed it. Phase 2 repopulates it (extension + sample + PROTOCOL.md + README.md + framebuffer-size-check tool).

Do NOT rebase off `main`. Work on `feature/webxr-bridge-v2`. Commit messages must include `(#139)`.

## Context to read in order

1. `CLAUDE.md` — project overview, build commands (`scripts\build_windows.bat`), logging conventions, capturing window screenshots section. Skip the macOS sections.
2. `docs/roadmap/webxr-bridge-v2-phase2-plan.md` — **the plan you're implementing**. Step-by-step, with a full JSON protocol v1 spec and the exact verification matrix.
3. `docs/roadmap/webxr-bridge-v2-plan.md` — the overarching architecture doc. Phase 2 section (tasks 2.1–2.9) is the canonical task list; the plan above just sequences it.
4. `src/xrt/targets/webxr_bridge/main.cpp` — Phase 1 bridge skeleton (~415 lines). You will extend this in step 4 of the phase 2 plan. Already handles `XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT` and `XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT` via `handle_event()`.
5. `src/xrt/targets/webxr_bridge/CMakeLists.txt` — Phase 1 build glue. Only links OpenXR loader.
6. `src/xrt/state_trackers/oxr/oxr_system.c:155-183` — the legacy compromise branch (0.5 × 1.0 view scale). Only touched if step 1 of the plan discovers Chrome clamps framebuffers.
7. `docs/architecture/extension-vs-legacy.md` — background on the legacy compromise path Chrome WebXR currently hits.
8. `docs/specs/XR_EXT_display_info.md` — what the bridge's OpenXR session actually sees. The JSON `display-info` message is a direct translation.
9. `docs/specs/swapchain-model.md` — canvas concept; informational for `computeFramebufferSize()`.
10. `test_apps/cube_hosted_d3d11_win/` — reference IPC OpenXR client.
11. (Optional) `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` grep `active_rendering_mode_index\s*=` — 7 mutation sites now broadcast `RENDERING_MODE_CHANGE` events, for your awareness of the complete event flow.

## Phase 2 goal in one paragraph

Produce a working end-to-end demo where (a) the user starts the DisplayXR service + bridge, (b) loads an MV3 extension into Chrome, (c) opens a three.js sample page, and (d) clicks Enter XR. Expected: the three.js scene renders **in the service-hosted window at the correct per-view atlas dims** (escaping the legacy 0.5 × 1.0 compromise), observes the bridge's `display-info` on connect, and re-settles cleanly when a V-key or 1/2/3-key press cycles the rendering mode mid-session. No input forwarding yet (Phase 3). No frame transport changes, ever.

## Execution order (see phase2-plan.md for detail)

1. **Framebuffer-size smoke test FIRST** (plan step 1). Before writing any extension or WebSocket code, build a standalone 50–80 line WebXR test page at `webxr-bridge/tools/framebuffer-size-check/index.html` that tries oversized framebuffers via both `XRWebGLLayer` (`framebufferScaleFactor: 4.0`) and `XRWebGLBinding.createProjectionLayer({ textureWidth, textureHeight })`. Log and report the returned dims. **Stop and report** after this step. The outcome determines:
   - Whether Phase 2 needs a runtime change (step 1b in plan).
   - Which WebXR API the sample uses.
   - Whether to ask the user about gating a legacy-recommendation widening behind `XRT_WIDEN_LEGACY_VIEWS`.

2. **Ask the user about library choices** before coding step 2. Specifically: (a) hand-rolled minimal WebSocket server vs uWebSockets vs libwebsockets, (b) `nlohmann/json` vs hand-rolled JSON, (c) WebSocket port 9014 availability (`netstat -ano | findstr :9014`). Recommendation in the plan: hand-rolled for both, single-client, ~300-500 LOC added to the bridge.

3. **Extend `src/xrt/targets/webxr_bridge/main.cpp`** with the WS server thread, outgoing message queue, and JSON serialization of `display-info` / `mode-changed` / `hardware-state-changed` messages. Phase 1 already has `handle_event()` with the two event types you need to serialize. On `XR_SESSION_STATE_EXITING` and Ctrl+C, shut down WS cleanly.

4. **Create `webxr-bridge/` at repo root** with this layout (Phase 1 deleted the old one):
   ```
   webxr-bridge/
   ├── README.md              # walkthrough
   ├── PROTOCOL.md            # JSON schema v1
   ├── extension/
   │   ├── manifest.json      # MV3, no nativeMessaging, two content scripts
   │   ├── icons/             # 16/48/128 placeholders
   │   └── src/
   │       ├── main-world.js      # navigator.xr Proxy + session.displayXR
   │       └── isolated-world.js  # WebSocket client + postMessage relay
   ├── sample/
   │   ├── index.html
   │   └── sample.js          # three.js via CDN
   └── tools/
       └── framebuffer-size-check/    # step 1 smoke test
   ```

5. **MV3 extension**: MAIN world content script wraps `navigator.xr.requestSession`, returns a `Proxy` over the real session exposing an added `displayXR` property. ISOLATED world script holds the WebSocket and relays messages via `window.postMessage` with strict origin checks. No nativeMessaging. No bundler. Plan has the exact `manifest.json` essentials.

6. **`session.displayXR` surface** (see plan for the full TS interface):
   - `displayInfo` (display dims, eye pos)
   - `renderingMode` (current mode catalogue entry + per-view dims)
   - `computeFramebufferSize(): { width, height }` — returns atlas-sized framebuffer for the current mode
   - Dispatches `renderingmodechange` + `hardwarestatechange` events on the underlying session object (via its `EventTarget`).

7. **three.js sample**: textured cube + skybox, `three` from CDN via importmap. On Enter XR: read `session.displayXR`, compute framebuffer size, create `XRWebGLLayer` or `XRProjectionLayer` (depending on step 1 outcome), run frame loop. On `renderingmodechange`: rebuild layer, call `session.updateRenderState({ baseLayer })`. Heavy comments. No input yet.

8. **`webxr-bridge/PROTOCOL.md`** documents the JSON schema v1 with version semantics and origin-check rules. **`webxr-bridge/README.md`** is a step-by-step walkthrough: build → install → start service → start bridge → load extension → open sample → Enter XR. Both are in the plan's step 9.

## Build / verify (plan's verification matrix)

Local Windows build only, per `CLAUDE.md`. Do NOT use `/ci-monitor`. Do NOT push to remote without asking.

```bat
scripts\build_windows.bat build
scripts\build_windows.bat installer
```

Uninstall the previous DisplayXR install before re-installing.

Verification matrix (all four must pass):

1. **Framebuffer-size smoke test.** `webxr-bridge/tools/framebuffer-size-check/index.html` runs in Chrome WebXR and reports the returned framebuffer dims. Outcome: either no runtime change needed (best case) or step 1b kicks in (ask user first).

2. **Bridge serves `display-info` on WS connect.** Start `displayxr-service.exe`, start `displayxr-webxr-bridge.exe`, open a WebSocket client (e.g. two lines in a browser console) to `ws://127.0.0.1:9014`, send `{"type":"hello","version":1,"origin":"test"}`, confirm a `display-info` message comes back with `displayPixelSize: [3840, 2160]` and both rendering modes (`"2D"`, `"LeiaSR"`).

3. **three.js sample renders at correct per-view dims.** Start service + bridge, load extension, open `webxr-bridge/sample/index.html`, click Enter XR. Scene should be **not horizontally squeezed** in 3D mode — compare a screenshot against `cube_hosted_d3d11_win` running in the same mode. This is the "we escaped the 0.5 × 1.0 legacy compromise" check.

4. **Mid-session mode change re-settles cleanly.** Same session as (3). Focus the service compositor window, press V once (or 1 / 2). Bridge logs `RENDERING_MODE_CHANGED previous=X current=Y`. Sample's page console logs `renderingmodechange`. Scene re-renders in the new mode with no visible glitch longer than ~30 ms. (The bridge's V-key event delivery is already verified by #142.)

Capture screenshots of the sample running in both modes (2D and LeiaSR) using the "Capturing Window Screenshots" procedure in `CLAUDE.md`.

## Commit plan

One commit at the end of Phase 2 on `feature/webxr-bridge-v2` unless step 1b was needed, in which case split into two reviewable commits (runtime change first, then bridge + extension + sample). See plan's "Commit plan" section for the exact message template.

## Things to ask the user before editing — do not assume

- **WebSocket library.** Hand-rolled minimal WS vs uWebSockets vs libwebsockets. Plan recommends hand-rolled.
- **JSON library.** `nlohmann/json` vs hand-rolled. Plan recommends hand-rolled.
- **Port 9014.** Confirm it is free on the dev machine before binding.
- **Runtime widening env-var name.** Only relevant if step 1 clamps. Plan suggests `XRT_WIDEN_LEGACY_VIEWS`. Confirm before committing any runtime change.
- **Extension ID stability.** Whether to allow any `chrome-extension://` origin in the bridge's origin check (Phase 2 recommendation) or tighten via `DISPLAYXR_EXTENSION_ID` env var.
- **Whether to open a separate tracking issue for Phase 2** or keep everything under #139.

## Things NOT to do

- Do NOT touch the WebXR frame pipeline. Frames flow through Chrome's native WebXR → OpenXR → service compositor path, zero-copy, untouched.
- Do NOT touch `cube_hosted_d3d11_win` or any other existing IPC app.
- Do NOT add `nativeMessaging` to the extension. The old bridge used it — dropped entirely.
- Do NOT add per-frame `U_LOG_W` calls in the bridge. `U_LOG_I` for recurring, `U_LOG_W` only for one-off init/error/lifecycle.
- Do NOT rebase off `main`. Work on `feature/webxr-bridge-v2` from commit `5c411cfdb` or later.
- Do NOT implement Phase 3 (input forwarding, `XR_EXT_app_owned_input`) in this session. That is a separate phase with its own plan.
- Do NOT widen the legacy recommendation in `oxr_system.c` unless step 1 proves it is necessary AND the user confirms the env-var gate plan.

Start by reading `docs/roadmap/webxr-bridge-v2-phase2-plan.md` in full, then re-read `src/xrt/targets/webxr_bridge/main.cpp` to understand what Phase 1 built. Then do the framebuffer-size smoke test FIRST and stop to report before building anything else.
```

---

## Notes for the person kicking this off

- Phase 1 is already merged on the branch. The bridge binary works end-to-end as a headless display-info reader; #142 is what unblocked event delivery and that is now verified.
- The single biggest unknown in Phase 2 is Chrome's framebuffer-clamping behaviour. That is why the plan forces a smoke test **before** the full sample or extension gets built. If Chrome clamps, the runtime widening path is small but needs user sign-off.
- The MV3 extension has zero permissions beyond two content scripts. WebSocket to `127.0.0.1` from an ISOLATED world content script does not need host permissions — confirmed in MV3 semantics. Document it in `PROTOCOL.md` so future reviewers don't second-guess.
- Phase 3 (`XR_EXT_app_owned_input`, qwerty disable widening, WndProc input ring buffer, IPC RPC `ipc_call_poll_window_input`) is out of scope for this prompt. Stop at Phase 2 — it ships as a usable "display-info-aware but standard input" bridge on its own.
- The bridge and extension both need to agree on JSON protocol version 1. Whenever the schema changes, bump the integer and reject mismatched versions on both sides. Plan's `PROTOCOL.md` is the single source of truth.
- `webxr-bridge/` at repo root was deleted in Phase 0. Phase 2 repopulates it. The agent should not be confused by its absence at session start.
