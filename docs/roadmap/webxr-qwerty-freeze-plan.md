# WebXR-in-Shell Qwerty-Freeze + Legacy-Offset Plan

**Branch:** `feature/webxr-in-shell` (10+ commits ahead of `origin`, Stage 3 +
three polish commits landed).
**Status:** Draft — design notes, not yet executed.
**Predecessor:** Stage 3 complete; this is the Stage 2 follow-up §1 plus a
related camera-offset issue surfaced during regression testing.

## Problems

Two coupled issues block interactive use of legacy Chrome WebXR (non-
bridge-aware) inside the shell — even though Stage 3's bridge-aware path
is working well:

### P1. Qwerty pose integration is frozen for legacy WebXR in shell

Once a bridge-aware Chrome session has attached and then exited (leaving
the `displayxr-webxr-bridge.exe` process running for future reuse), the
qwerty driver can end up with its pose integration permanently gated off
or oscillating. Effect: WASD / Alt-drag don't move the camera, qwerty
controllers don't respond, shell-mode legacy WebXR renders from a fixed
"frozen" pose.

Code anchors (verified):

- `src/xrt/drivers/qwerty/qwerty_device.c:34` — file-scope static
  `g_qwerty_bridge_relay_active`, single setter at `:37`
  (`qwerty_set_bridge_relay_active`).
- `src/xrt/drivers/qwerty/qwerty_device.c:350-370` — the freeze gate in
  `get_tracked_pose`. When active it zeros all deltas and returns the
  cached pose, short-circuiting the WASD / mouse-delta integration
  below.
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:8272-8284` —
  single caller of `qwerty_set_bridge_relay_active`. Flips on a
  `bridge_client_is_live` transition, but is reached from **every
  client's** `compositor_layer_commit`, using `c->render.hwnd` as the
  HWND to check for `DXR_BridgeClientActive`.

Consequences:

- `bridge_client_is_live` returns `true` iff
  (`g_bridge_relay_active` — bridge OpenXR session alive) AND
  (`c->render.hwnd` has the `DXR_BridgeClientActive` prop). The prop
  is set on the compositor HWND in the bridge's `bridge-attach` WS
  handler and cleared on WS disconnect / `bridge-detach`.
- With multiple concurrent clients (handle app + Chrome; or sequential
  bridge-aware then legacy), the per-frame computation uses different
  `c->render.hwnd` values. The single global `s_last_bridge_live`
  oscillates, calling `qwerty_set_bridge_relay_active(…)` in both
  directions.
- The qwerty freeze is a process-wide global, so any transient `true`
  observation from *some* client's frame freezes qwerty for *every*
  other client too — including a legacy WebXR session running
  concurrently in a different shell slot.

### P2. Legacy WebXR in shell starts at wrong camera / viewer offset

User report: when legacy Chrome WebXR opens in a shell slot, the
initial camera pose and convergence plane are wrong — the cube (or
whatever scene the page renders) sits mispositioned relative to the
shell window. Handle apps and bridge-aware WebXR don't have this.

Code anchors:

- `src/xrt/drivers/qwerty/qwerty_device.c:52` —
  `QWERTY_HMD_DISPLAY_POS (0, 1.6f, -2.0f)`: 2 m behind the display,
  1.6 m up. Sensible for an HMD simulator, wrong for a Leia desktop
  display where the user sits ~0.6 m in front (`nominal_viewer_z`).
- `src/xrt/state_trackers/oxr/oxr_session.c:1240+` — Stage 2's path
  for populating `adj_eyes` from DP-tracked eye positions, with the
  nominal-viewer fallback when no DP eye data is available.
- `src/xrt/drivers/qwerty/qwerty_device.c:656` — default
  `nominal_viewer_z = 0.6f`.

Hypothesis: for legacy WebXR, Chrome submits at its own default starting
pose (WASD/look offset), and the runtime adds qwerty's (0, 1.6, -2.0)
HMD origin on top. Result: the "shell eye" ends up 2 m behind the
display, not at ~0.6 m in front. Needs confirmation from logs.

## Scope

- **In scope.** Make legacy Chrome WebXR in shell respond to WASD /
  Alt-drag / mouse look, starting from a sensible camera offset that
  puts the viewer at roughly nominal-viewer distance in front of the
  shell window (not 2 m behind an imaginary HMD).
- **In scope.** Preserve bridge-aware WebXR behavior: the sample owns
  input and qwerty should stay frozen while a bridge-aware session is
  actively attached (the whole point of the relay).
- **In scope.** Preserve handle-app behavior: WASD / camera mode / C
  / T / V all continue to work in shell and standalone.
- **Out of scope.** Bridge-aware gamma / color shift (P3 follow-up —
  separate plan doc).
- **Out of scope.** Live resize / pose events during an active session
  (Stage 4).
- **Out of scope.** Multi-bridge-aware-tab concurrency (bridge already
  single-WS-client).

## Architectural note — qwerty affects the scene, not the shell

A refinement worth stating upfront: qwerty WASD / Alt-drag should move
the camera **inside the legacy WebXR page's scene**, not move the
shell window in display space. The shell window is anchored by
`mc->slot[].window_pose` (owned by the multi-compositor and user-drag
hit-tests), which is independent of qwerty state. Qwerty integrates
into the HMD device's pose, which flows through `xrLocateViews` into
`view_pose.position`, which Chrome consumes as the camera pose in the
page's world coordinates.

Mechanical consequence: when the user presses W inside a shell-hosted
legacy WebXR, Chrome's scene should appear to slide *past* them; the
shell window stays fixed in display space because its pose is separate
state. That's already what the architecture does — we just need qwerty
unfrozen and started from a sensible offset for the scene to feel
right.

Corollary: nothing in D1 / D2 should attempt to move the shell window
via qwerty input. If at any point it looks like the shell window is
translating during WASD, something has gone wrong in the plumbing
(likely qwerty state accidentally feeding `window_pose` through Stage 2
eye-delta math — shouldn't happen, but worth watching in regression
testing).

## Design sketch

### D1. Tighten the qwerty freeze gate

**Current:** global flag flipped from arbitrary clients' per-frame
`layer_commit`. Side-effect: flag races across sessions.

**Target:** freeze qwerty iff a bridge-aware session is actively using
the relay (WS client connected, extension attached). Independent of
which *other* sessions' `layer_commit` ran last.

Candidate approaches — evaluate, pick one:

1. **Drive the freeze from the WS client's connect/disconnect event in
   the bridge**, not from per-frame compositor passes. The bridge
   already sets `DXR_BridgeClientActive` on `bridge-attach` WS message
   and clears on WS disconnect / `bridge-detach`. Mirror the same
   transitions into `qwerty_set_bridge_relay_active` via a new IPC
   method, or piggyback on an existing event. Removes the per-frame
   racing entirely.

2. **Make the freeze gate per-session** — qwerty tracks which sessions
   are bridge-relay-owning and only freezes poses delivered to those
   sessions. Implementation cost: qwerty doesn't currently know which
   session is asking; requires plumbing session identity through
   `xdev_get_tracked_pose`. Larger change.

3. **Compute the gate in the compositor from authoritative state**
   (not from `c->render.hwnd` of the arbitrary caller). Walk
   `mc->clients[]`, look for any slot whose client has a
   `DXR_BridgeClientActive`-owning session, compute the gate from
   that. Keep the existing single global qwerty flag but drive it
   from a single source of truth each frame. Small change, fixes the
   oscillation.

Recommend option 3 for v1 (minimal surgery, unblocks the legacy-WebXR
use case). Option 1 is cleaner long-term but needs a new IPC message or
shared-mem flag. Option 2 is over-engineered for this stage.

### D2. Sensible legacy-WebXR starting offset in shell

The qwerty HMD `QWERTY_HMD_DISPLAY_POS (0, 1.6, -2.0)` is hard-coded
for HMD-style virtual display. In shell mode the display lives in front
of the user at `nominal_viewer_z` (~0.6 m), not 2 m behind them.

Investigation first:

- Log Chrome's initial `rawViews[].pose.position` when a legacy WebXR
  session starts in shell. If Chrome's own default + qwerty origin
  compound to `(0, 1.6, -2)` we know where to intervene.
- Check whether Stage 2's `oxr_session_locate_views` already offsets
  eyes using `nominal_viewer_position` from `xrt_system_compositor_info`
  — if so, the issue may be Chrome-side default, not runtime-side.

Candidate fix paths:

1. Initialize qwerty HMD `pose.position` from the device's nominal
   viewer position when running in shell mode, not the hard-coded
   `(0, 1.6, -2.0)`. Same device-init path `qwerty_system_create` or
   equivalent — seed the HMD with `(0, nominal_viewer_y, nominal_viewer_z)`
   instead.

2. Override the starting offset only when the session is a legacy
   shell-mode IPC client (not a standalone HMD-sim use case). Requires
   the qwerty driver to know which sessions are shell-hosted — adds
   coupling qwerty currently doesn't have.

Recommend (1) unconditionally — the DP's nominal viewer is the correct
origin for any user-sitting-in-front-of-display scenario, including
standalone HMD-sim. HMD emulator users who want (0, 1.6, -2) can set
an env var or a qwerty-pose-reset hotkey post-init.

## Verification

Hardware: Leia SR display, dev machine, Chrome with WebXR enabled.

| Check | Expected |
|---|---|
| Legacy WebXR (e.g. `immersive-web.github.io/webxr-samples/immersive-vr-session.html`) in shell → WASD + Alt-drag | Camera moves / looks around. |
| Bridge-aware sample in shell, page live | Qwerty stays frozen (page owns input). |
| Same bridge-aware page — close + reopen → qwerty wake-up | Qwerty resumes within one frame of WS disconnect / `bridge-detach`. |
| Handle app (`cube_handle_d3d11_win`) in shell | WASD / C-mode / V-key / all unchanged. |
| Two-slot mix: handle app + legacy WebXR in shell | Both respond to qwerty simultaneously (no oscillation-induced stalls). |
| Legacy WebXR initial starting pose | Camera origin is near the nominal viewer position in front of the shell window; cube appears centered at the convergence plane, not 2 m behind it. |
| Regression: legacy WebXR with shell off (pre-Stage-2 physical-display Kooima path) | Behavior unchanged from before. |

## Pitfalls

1. **Don't confuse the bridge process existence with a live bridge
   session.** `g_bridge_relay_active` turns true the moment any
   bridge-relay session exists (bridge exe running + OpenXR session
   created). But the *interactive* gate is `bridge_client_is_live`
   which additionally requires `DXR_BridgeClientActive`. Use the
   latter, not the former, when deciding to freeze qwerty.

2. **The compositor's `c->render.hwnd` is not a consistent HWND
   across clients.** For shell-mode clients it may be the shared
   compositor HWND or a per-client atlas-only HWND depending on
   setup. Don't assume `GetPropW(c->render.hwnd, L"DXR_BridgeClientActive")`
   is the same as querying `sys->compositor_hwnd` — that's the
   root of today's oscillation.

3. **Qwerty freeze affects ALL sessions.** Because qwerty is a
   singleton device shared across every session, making the freeze
   per-session means either plumbing session identity through
   `get_tracked_pose` (intrusive) or accepting "freeze if ANY live
   bridge session exists" semantics. Option 3 above accepts the
   latter; document that handle apps will briefly freeze alongside
   bridge-aware ones, but they share the same qwerty state anyway
   so the distinction is cosmetic.

4. **Starting offset = (0, 1.6, -2) is used by more than shell.**
   Any tool that spins up qwerty without a DP (qwerty-only test
   apps, gui, cli diagnostics) expects HMD-sim semantics. Confirm
   before changing the default that those paths also have a
   sensible `nominal_viewer_*` available; fall back if not.

5. **Stage 3's `displayxr-webxr-bridge-ipc` secondary IPC connection
   is NOT a bridge-relay session.** Don't let it confuse any code
   that enumerates bridge-relay clients. Its `application_name`
   distinguishes it; the bridge's OpenXR session alone sets
   `is_bridge_relay`.

6. **Test Alt+Tab re-entry.** Stage 0b (`feedback_latched_alt_tab`
   follow-up from earlier) already cleared qwerty modifier state on
   `WM_KILLFOCUS`; confirm this new change doesn't re-stick any
   latched modifiers when the freeze lifts.

## Files likely touched

| File | Why |
|---|---|
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | Replace per-client freeze-flip with authoritative compute (D1 option 3). |
| `src/xrt/drivers/qwerty/qwerty_device.c` | Change HMD starting pose default to nominal-viewer-derived (D2 option 1). |
| `src/xrt/drivers/qwerty/qwerty_device.h` / `_interface.h` | If the init signature needs nominal-viewer args. |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (init) | Log initial qwerty pose for P2 root-cause confirmation before changing. |

## Staging

1. **Diagnose.** Add one-shot `U_LOG_W` at `qwerty` HMD init logging
   initial `pose.position` and `nominal_viewer_*`. Add another log at
   the first `bridge_client_is_live` transition showing which HWND
   was queried and which prop was found. Rebuild, deploy, repro the
   user's scenarios, read the log. No code behavior change; just
   confirms the hypotheses above.
2. **D1 fix — qwerty freeze gate.** Implement option 3 (authoritative
   per-frame compute from `mc->clients[]`, not arbitrary caller hwnd).
   Commit. Verify legacy-WebXR movement in shell.
3. **D2 fix — starting offset.** Initialize qwerty HMD pose from
   nominal viewer at qwerty_system_create. Commit. Verify initial
   camera pose is correct.
4. **Regression sweep** per the verification table.

## Definition of done

- [ ] Legacy WebXR in shell: WASD / Alt-drag moves the camera.
- [ ] Legacy WebXR in shell: initial camera pose is near nominal
      viewer (not 2 m behind display).
- [ ] Bridge-aware WebXR: qwerty stays frozen while the WS client
      is attached, wakes up within ~1 frame of `bridge-detach` /
      WS disconnect.
- [ ] Handle apps in shell: WASD / camera mode / C / T / V / all
      hotkeys unaffected.
- [ ] Two-slot mix (handle + legacy WebXR) works simultaneously.
- [ ] Nothing pushed; commits stacked on `feature/webxr-in-shell`.
- [ ] Stage 2 follow-ups §2 (color), §3 (launcher empty-state), §4
      (window pose reset) not regressed.

## After this ships

The remaining Stage 2 follow-up is the bridge-aware color/gamma
shift (blue→purple). That's a compositor blit color-space investigation
— a separate plan doc with its own agent prompt. After that: Stage 4
(live resize during session) closes the parent plan.
