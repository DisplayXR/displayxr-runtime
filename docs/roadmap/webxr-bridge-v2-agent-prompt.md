# WebXR Bridge v2: Agent Prompt

Use this prompt to start a new Claude Code session on a Windows machine with the Leia SR SDK and a 3D display attached, to implement WebXR Bridge v2 on branch `feature/webxr-bridge-v2`.

---

## Prompt

```
I'm implementing WebXR Bridge v2 for DisplayXR — a metadata/control sideband that makes WebXR pages behave like handle apps (display-info aware, rendering-mode-event aware, custom input, qwerty suppressed) while still rendering through Chrome's native WebXR → OpenXR → service compositor path. Frames stay on the existing zero-copy DXGI-handle path — this work does NOT touch the frame pipeline.

## Context (read in order)

1. `CLAUDE.md` — project overview, build commands, architecture, Windows test-app conventions, debug-log conventions, and the "Capturing Window Screenshots" section I will reference for verification
2. `docs/roadmap/webxr-bridge-v2-plan.md` — **the plan you're implementing** (full architecture, phased task lists, file paths, verification steps)
3. `docs/specs/XR_EXT_display_info.md` — the extension the bridge's metadata session will enable
4. `docs/architecture/extension-vs-legacy.md` — explains the legacy-app compromise the WebXR frame path currently hits
5. `docs/specs/swapchain-model.md` — canvas concept, window-relative Kooima (informational; the sample doesn't compute projections itself)
6. `src/xrt/state_trackers/oxr/oxr_system.c:155-183` — the exact legacy 0.5×1.0 compromise branch the sample app is going to bypass
7. `src/xrt/state_trackers/oxr/oxr_session.c:855-885` — where `XrEventDataRenderingModeChangedEXT` is pushed (bridge consumes this)
8. `src/xrt/state_trackers/oxr/oxr_session.c:2604-2615` — where qwerty auto-disable fires for external-window apps (Phase 3 widens this condition)
9. `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:8943-8969` — window-relative Kooima (informational)
10. `test_apps/cube_hosted_d3d11_win/` — reference minimal IPC OpenXR client to model the bridge host after (minus the graphics binding)

## Branch

You are on branch `feature/webxr-bridge-v2`. The branch already exists with Phase 0 (deletion of the old bridge) committed on it. Pull it and work from there. No `-ci` suffix — this is local Windows development, not remote CI.

Tracking issue is #139. Include `(#139)` in every commit message.

## What we're building

A new Chrome MV3 extension + native bridge process (`displayxr-webxr-bridge.exe`) acting as a metadata sideband. The bridge is its own tiny OpenXR client with `XR_EXT_display_info` enabled that relays display info, rendering-mode-change events, and hosted-window input events to the browser over a loopback WebSocket on `127.0.0.1:9014`. A Chrome extension wraps `navigator.xr` with a `session.displayXR` namespace. A three.js sample demonstrates the pattern.

Two OpenXR sessions coexist against the same service: Chrome's (the renderer, unchanged) and the bridge's (metadata only, no swapchain). Service is multi-client capable post-#116.

Three phases — each independently verifiable and mergeable. Do them in order.

## Phase 1 — Bridge host skeleton

No extension, no WebSocket, no runtime changes. Just the OpenXR client that reads display info and logs events, and deletion of the old bridge.

**Tasks:**

1. **Delete the old `webxr-bridge/`** — **already done** in the first commit on this branch (Phase 0). You should find `webxr-bridge/` empty or missing when you clone. If anything from the old bridge is still present (`native-host/`, `extension/`, `package.json`, `build.sh`, etc.), stop and ask — that would mean something went wrong with the Phase 0 commit.

2. **Create `src/xrt/targets/webxr_bridge/`** with `main.cpp` and `CMakeLists.txt`. Model the CMake after `test_apps/cube_hosted_d3d11_win/CMakeLists.txt` but drop all D3D11/graphics dependencies. Only link the OpenXR loader, standard C/C++, and whatever logging helpers the runtime uses.

3. **Write a minimal OpenXR client in `main.cpp`** that:
   - Creates `XrInstance` with `XR_EXT_display_info` enabled
   - Creates an `XrSystemId` and queries system properties + the `XrSystemDisplayInfoPropertiesEXT` chain, logging everything
   - Enumerates view configurations + view configuration views per mode, logging per-mode dims
   - Creates an `XrSession`. **Important question to resolve first**: does the DisplayXR runtime expose `XR_MND_headless` or equivalent? Check `src/xrt/state_trackers/oxr/oxr_instance.c` (or wherever extensions are listed). If yes, use it. If no, fall back to a no-op D3D11 graphics binding — create a throwaway `ID3D11Device`, pass it via `XrGraphicsBindingD3D11KHR`, and never create a swapchain or submit a frame. This is the minimum work to get to `xrPollEvent`.
   - Runs `xrPollEvent` in a loop on the main thread. Log every event. On `XrEventDataRenderingModeChangedEXT`, re-query `xrEnumerateViewConfigurationViews` and log the new per-view dims.
   - Clean exit on Ctrl+C.

4. **Wire into `scripts\build_windows.bat`** so the `build` and `all` targets produce `_package\bin\displayxr-webxr-bridge.exe`.

5. **Coexistence verification** (the real goal of Phase 1):
   - `scripts\build_windows.bat build`
   - Terminal A: `_package\bin\displayxr-service.exe`
   - Terminal B: `_package\bin\displayxr-webxr-bridge.exe` — should log display info dump
   - Terminal C: open a WebXR demo in Chrome (e.g. https://immersive-web.github.io/webxr-samples/) and click Enter VR
   - Cycle render modes using whatever service hotkey is active today
   - Confirm: Chrome renders, bridge session stays alive, both sessions observe the same mode-change events via their respective event queues, no crashes, no lock contention

   Use the "Capturing Window Screenshots (Autonomous Testing)" procedure in `CLAUDE.md` to capture the service-hosted window and confirm the Chrome WebXR scene is visible there.

6. **Commit.** Message:
   ```
   Scaffold WebXR Bridge v2 host + delete old bridge (#<issue>)

   - New target src/xrt/targets/webxr_bridge/, links OpenXR loader only
   - Enables XR_EXT_display_info, logs display info + rendering mode events
   - Coexists with Chrome's native WebXR session against the same service
   - Old macOS-only WebSocket-frame bridge fully removed
   - Phase 1 of docs/roadmap/webxr-bridge-v2-plan.md
   ```

**Stop and report** after Phase 1 verification passes. Do not start Phase 2 until the user confirms. Include in your report:
- Whether headless sessions are supported, or which binding workaround you used
- Screenshots of the bridge's display info dump
- Screenshots of the Chrome WebXR scene in the service-hosted window
- Which service hotkey currently cycles render modes (the plan is vague on this; document what you found)
- Any lock-contention or crash observed during coexistence testing

## Phase 2 — WebSocket metadata server, MV3 extension, three.js sample

No runtime changes. Bridge + extension + sample only.

**Critical early check (task 2.3 in the plan):** before building the full sample, write a 50-line test WebXR page that passes an oversized `framebufferScaleFactor` (or uses `XRWebGLBinding.createProjectionLayer({ textureWidth, textureHeight })` with large values) and logs `layer.framebufferWidth/Height`. If Chrome clamps to the legacy 0.5×1.0 recommendation, implement the fallback: widen `info->legacy_view_width_pixels` / `legacy_view_height_pixels` at `src/xrt/state_trackers/oxr/oxr_system.c` ~line 155-183 to cover the largest rendering mode, and verify the compositor handles this gracefully. Document the finding in a comment on task 2.3 and in the phase 2 commit message.

After that, proceed through tasks 2.1, 2.2, 2.4–2.9 in the plan. Key points:
- WebSocket bound to `127.0.0.1` only, reject any non-loopback origin
- Two-script MV3 injection (MAIN world `navigator.xr` wrapper + ISOLATED world WebSocket client)
- No build step for the extension — loadable directly via `chrome://extensions`
- No `nativeMessaging` permission (dropped from old bridge)
- three.js from a CDN, no bundler, no `package.json`
- `session.displayXR.computeFramebufferSize()` is where the "escape the legacy 0.5×1.0" magic happens — it reads the current mode from the bridge and returns pixel dims the page passes to `XRWebGLLayer`

**Stop and report** after Phase 2 verification passes. Include:
- Whether Chrome honours oversized framebuffers (task 2.3 result) and which code path you landed on
- Screenshot of the sample rendering correctly in a non-SBS mode (where 0.5×1.0 would be visibly wrong)
- Screenshot of the sample re-settling cleanly after a mid-session mode change

## Phase 3 — XR_EXT_app_owned_input and input forwarding

Runtime changes start here. Follow tasks 3.1–3.9 in the plan in order.

Key points:
- New extension strictly opt-in — `cube_hosted_d3d11_win` and every other existing IPC app must be untouched
- Qwerty auto-disable broadening at `oxr_session.c:2604-2615` must restore `qwerty_set_process_keys(true)` on session destroy
- IPC RPC `ipc_call_poll_window_input` added to `proto.per`; regenerate sibling files per repo convention
- Per-session input ring buffer lives in `comp_d3d11_service.cpp` alongside the existing WndProc; size 256, overflow drops oldest
- One XInput poll per compositor frame, not one per event — produces synthetic gamepad-delta events
- Extension synthesizes `MouseEvent` / `KeyboardEvent` via `document.dispatchEvent`, and shims `navigator.getGamepads()` so vanilla three.js / pointer-lock / gamepad code works unmodified
- Sample demonstrates WASD-driven camera with qwerty head pose confirmed disabled

**Regression check** at the end: run `_package\bin\cube_hosted_d3d11_win.exe` against the same service with neither the bridge nor the extension running. Confirm it behaves identically to pre-Phase 3 — qwerty still moves its head pose, no new RPC fires on its session. If this regression fails, the `XR_EXT_app_owned_input` gating is wrong.

## Conventions (read CLAUDE.md for the full list)

- **Logging:** `U_LOG_W` only for one-off init/error/lifecycle events. `U_LOG_I` for anything recurring. Never per-frame `U_LOG_W`.
- **Commit messages:** always reference the GitHub issue number.
- **Don't `--no-verify`**, don't skip hooks. Investigate failures.
- **Don't `--amend`.** Create new commits on hook failures.
- **Prefer local builds** via `scripts\build_windows.bat build` over `/ci-monitor`. CI is for feature branches ending in `-ci`, which this branch deliberately does not.
- **Never add per-frame `U_LOG_W` calls** — causes log bloat.
- **Spec docs** (`docs/specs/XR_EXT_app_owned_input.md`) should match the format of `XR_EXT_display_info.md`.

## What to ask the user

If any of these come up, stop and ask before deciding:
- Whether to enable the new extension in the standard extension list or behind a build flag
- The exact name / identifier for `XR_EXT_app_owned_input` if "app_owned_input" conflicts with something
- Whether the WebSocket port `9014` clashes with anything else on the Windows box
- Whether to open a tracking GitHub issue or reuse an existing one
- If Chrome's framebuffer-clamping behaviour requires a more invasive runtime change than widening `legacy_view_width_pixels`

Start with Phase 1. Read the plan file first. Work step by step, marking tasks done as you finish them, and stop at each phase boundary for verification and user confirmation before continuing.
```

---

## Notes for the person feeding this prompt

- The plan file is the source of truth for architecture and task lists. The prompt above just sequences the work and points at verification checkpoints.
- Phases are independently mergeable; you can ship Phase 1 on `main` and continue Phase 2 on the same branch, or split them into separate PRs.
- Phase 2 has one risky verification (`XRWebGLLayer` framebuffer clamping) that determines whether we stay out of the runtime for Phase 2 or not. Do that check first.
- Phase 3 is the only phase with runtime changes. If the agent gets stuck there, consider stopping at Phase 2 and shipping that as a usable "display-info-aware but standard input" bridge.
- The whole WebXR frame pipeline is out of scope. Frames flow through Chrome's native WebXR path, unchanged.
