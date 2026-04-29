---
status: Accepted
date: 2026-04-28
---
# ADR-016: Workspace Controllers Own Their Tray Surface and Lifecycle

## Context

DisplayXR ships a system tray icon (in `displayxr-service.exe`) that surfaces runtime state and lets the user manage the workspace controller — the DisplayXR Shell today, and any future third-party workspace app registered via the `XR_EXT_spatial_workspace`-family extensions. The service currently hardcodes a `Workspace Controller` submenu with **Enable / Auto / Disable** radio items, and the orchestrator (`service_orchestrator.c`) implements those verbs directly:

- **Enable** spawns the controller binary and keeps it running.
- **Auto** registers a `Ctrl+Space` low-level keyboard hook that spawns the controller on demand.
- **Disable** issues `TerminateProcess` on the controller, hard-killing it from outside its own process model.

Phase 2.0 of the brand-separation milestone (`feature/shell-brand-separation`) decoupled the runtime from the literal "displayxr-shell" name: the controller path is discoverable, the friendly tray label comes from a sidecar manifest, and `workspace_activate` is gated by orchestrator-PID match instead of `applicationName == "displayxr-shell"`. But the service still **owns the menu structure and the verbs themselves**. That is workable while there is exactly one controller (the DisplayXR Shell) whose teardown happens to be "killing the process is fine." It breaks as soon as a third-party workspace controller registers and has any of:

- Unsaved state to flush (open user windows, in-flight network requests, IPC peers expecting a clean disconnect).
- A different desired action set — "Disable" might be wrong; "Lock", "Sleep", "Switch User", or vendor-defined verbs might be right.
- A different set of compositor and OS resources whose lifecycle only the controller knows how to release.

A symptom of the current architecture surfaced during Phase 2.0 functional testing: clicking **Disable** hard-kills the shell process via `TerminateProcess`, but the workspace D3D11 window — owned by the *service* compositor, not the controller — keeps rendering the controller's last frame indefinitely, because nothing tells the compositor that workspace mode is over. The compositor's existing deactivate path is reached today only when the workspace window receives `WM_CLOSE` (current code: ESC / window close-button on the compositor window), and that path is itself slated to change for unrelated UX reasons: ESC must stop closing the shell and revert to closing only hosted windows in direct-IPC apps. Patching the lingering-window symptom from the service side (synthetic `WM_CLOSE`, cross-module compositor callback, polled PID heartbeat) would couple the runtime more tightly to one controller's UX rather than less.

## Decision

The runtime owns the tray icon and renders the tray menu. Workspace controllers — first-party (DisplayXR Shell) and third-party — own:

1. **The tray menu items** they want to expose: their submenu's structure, labels, radio/checkbox state.
2. **The handlers** for those items: what each click does.
3. **Their own lifecycle**, including how to respond to a "Disable" / "Quit" / equivalent click. The service does not call `TerminateProcess` on a registered controller; it relays the click to the controller via IPC and the controller exits itself.

The service's responsibilities reduce to:

- Detecting that a controller is installed (manifest discovery — already shipped in Phase 2.0).
- Receiving menu definitions from the controller via the workspace extension protocol.
- Rendering those items in the tray submenu and routing OS click events back to the controller.
- Forwarding clicks back to the controller as IPC messages.
- Hard-killing the controller **only as a last-resort fallback** if the controller is unresponsive within a timeout, or if no controller is registered.

The current `Enable / Auto / Disable` set, the `Ctrl+Space` keyboard-hook-based spawn, and the orchestrator's `TerminateProcess` path remain the **transitional implementation** until the controller-driven menu protocol lands. They are first-party-shell defaults that get stripped out the moment a third-party controller registers its own menu definitions.

## Consequences

- The runtime stops being opinionated about workspace verbs. "Disable" is no longer a runtime concept — it is a controller-defined verb that some controllers may expose and others may not.
- Third-party workspace controllers can ship without the runtime knowing anything about their UX semantics. The runtime's only invariant about controllers is: "there is at most one active, it has a manifest, it follows the workspace extension protocol."
- The lingering-window bug present at the time this ADR is filed — Disable click hard-kills the controller, the compositor window keeps the last frame on screen — is **resolved by construction** under the new model: the controller deactivates workspace mode itself before exiting (it has an IPC channel, it knows how), the compositor cleans up through the existing in-protocol path, and no service-side compensation is needed.
- Until the cooperative-shutdown protocol ships, the runtime retains the current `TerminateProcess`-on-Disable behavior with the documented limitation that the compositor window may briefly outlive the controller. This is acceptable as a transitional state because (a) only the first-party shell is affected, (b) the next workspace event (`Ctrl+Space` re-spawn or another Enable click) clears the stale frame, and (c) the proper fix is a Phase 2.A+ deliverable, not a hotfix on the brand-separation branch.
- The Phase 2.0 fix to the `workspace_watch_thread_func` ↔ `terminate_child` double-close race (`DuplicateHandle` on the watch thread's wait handle) stands under either model: any time one component watches a process and another terminates it, those two paths must own independent kernel handle references. The fix is forward-compatible with cooperative shutdown.
- `XR_EXT_spatial_workspace` (or whatever the extension ends up named) must include at least: (1) a way for the controller to register tray menu definitions; (2) a way for the service to forward menu clicks to the controller; (3) a "request orderly shutdown" message the controller can use as a single internal entry point for teardown invoked from any source — tray click, system shutdown, OEM device-management agent, debugger detach, etc. Symmetry: the same message a third-party launcher would send to politely ask the controller to leave.
- The transitional first-party shell defaults (`Enable / Auto / Disable`, `Ctrl+Space`, `TerminateProcess`) move from runtime-owned code into shell-owned code as the protocol lands. The runtime keeps the *fallback* path (last-resort kill) and the *render* path (drawing whatever menu definitions the controller supplied). Nothing else.

## Related

- Phase 2.0 (`feature/shell-brand-separation`): manifest-driven controller detection, PID-match auth, `service_orchestrator_get_workspace_pid()`.
- Phase 2.A+: workspace extension surface migration; this protocol lands here.
- `docs/roadmap/spatial-workspace-extensions-plan.md` — overall plan and phase boundaries.
- `docs/roadmap/workspace-runtime-contract.md` — the controller↔runtime IPC contract that this protocol extends.
- `docs/roadmap/spatial-workspace-controller-detection.md` — manifest schema (the discovery half of this story).
