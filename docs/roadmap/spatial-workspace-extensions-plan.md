# Spatial Workspace Extensions — Plan

**Status:** Draft, 2026-04-27. Branch `feature/shell-brand-separation`.

## Goal

Decouple the DisplayXR runtime from the proprietary shell so that:

1. The **public runtime** stops reading like an SDK for one specific proprietary application. Today the public runtime exposes 14 `shell_*` IPC RPCs, a `shell_mode` field in the core compositor interface, hardcoded `displayxr-shell.exe` spawn paths, and ~217 internal mentions of "shell" in the D3D11 service compositor — even though the shell binary itself is stripped from public releases. This implies a single-vendor stack to outside readers.
2. **Third parties** can build their own privileged compositor clients — vertical workspaces (medical, industrial, trading), kiosks / signage, OEM-branded shells, test harnesses, AI-agent drivers — using a public extension surface. The DisplayXR Shell becomes one reference implementation of a workspace controller, not the only thing the runtime knows how to serve.
3. The runtime repo can eventually be **severed** from the shell repo so external contributors can PR directly against the public runtime, eliminating the current `apply-public-pr.sh` apply-from-public-to-private dance.

## Terminology

The privileged client that arranges multiple apps in a shared 3D space, drives layout, registers launcher tiles, and captures frames is called a **workspace controller** (or just **workspace** when context is clear). The runtime mode in which the compositor serves such a client is **workspace mode**.

**Why `workspace`** — alternatives considered:

- `host` — collides with the existing `_hosted` app class (where the runtime owns the window). The shell is itself a hosted-style consumer of the runtime, so calling its role "host" produces unreadable architectural docs.
- `composer` — too close to `compositor`, which is already overloaded as the per-API graphics module.
- `window_manager` / `wm` — accurate (matches X11/Wayland tradition), but presupposes draggable windows. Doesn't fit kiosks, single-app fullscreen layouts, or AI-agent drivers that don't surface UI.
- `orchestrator` — already taken by `service_orchestrator.c` for a different concern (spawning child processes).
- **`workspace`** — covers all the use cases above, "spatial workspace" already appears in product copy, doesn't collide with anything in the codebase, doesn't presuppose windows or chrome.

**Two-tier vocabulary**:
- The DisplayXR Shell remains a brand and a product (binary, marketing, OEM relationships).
- "Workspace controller" is the architectural role the shell plays — and that any third-party privileged client could play.

## Phasing

The work splits into three phases with sharply different costs and risk profiles. Each is independently shippable.

### Phase 1 — Boundary rename (this branch, ~half day)

Rename only the symbols that **cross the runtime/shell process boundary**. Internal compositor code keeps its current names. The 217 internal "shell" mentions in `comp_d3d11_service.cpp` are deliberately left alone — most of them belong to policy that will move out of the runtime entirely in Phase 2, so renaming them now would be throwaway churn.

Scope is ~30 callsites: the IPC namespace, the public functions in `comp_d3d11_service.h`, one field in `xrt_compositor.h`, the `DISPLAYXR_SHELL_SESSION` env var, and the hardcoded `displayxr-shell.exe` spawn path. Detailed map below.

After Phase 1: the public runtime's IPC protocol, core interface, and orchestrator config all read as neutral. A reader can no longer infer "this runtime is built for one specific proprietary shell" from any public surface. The shell binary still lives in `targets/shell/` and is still stripped from public releases — Phase 1 doesn't change topology.

### Phase 2 — Mechanism vs policy split (separate branches, weeks)

Phase 2 is preceded by **Phase 2.0** — two short pieces of architectural prep that gate the larger code migrations. Both modify the orchestrator and tray; both are designed in their own docs:

- **[Workspace controller detection](spatial-workspace-controller-detection.md)** — orchestrator detects whether a workspace controller binary is installed and reads its sidecar `.controller.json` manifest for display metadata. Tray submenu shows / hides accordingly. Makes the runtime a real platform: bare-runtime install (no shell) gives a useful product (OpenXR + WebXR bridge) with no Workspace submenu; installing the shell adds the submenu.
- **[Workspace activation auth handshake](spatial-workspace-auth-handshake.md)** — replaces the literal `"displayxr-shell"` `application_name` match in `ipc_server_handler.c:296` with an orchestrator-PID match. Removes the last brand coupling in the runtime.

After Phase 2.0 the runtime ships standalone, recognises any installed workspace controller by its manifest, and grants the activation right by spawn-PID rather than by hardcoded name. With that scaffolding in place, Phase 2 then moves policy out of the compositor:

Move policy (which apps exist, what tiles to show, layout-preset semantics, registered_apps.json management, capture client lifecycle) **out of the compositor** into the workspace controller process behind the workspace extensions. Mechanism (multi-client atlas composition, raycast hit-test against window planes, offscreen capture, IPC plumbing) **stays in the runtime** under neutral names.

This is where the 162 internal mentions in `comp_d3d11_service.cpp` (down from 217 after Phase 1) get cleaned up — most by deletion (the code moves to the workspace process), the rest by renaming once they're clearly mechanism with no policy embedded. The line-by-line classification is in the [Phase 2 audit](spatial-workspace-extensions-phase2-audit.md), which proposes 8 sub-steps (2.A through 2.H) ordered lowest-blast-radius-first.

Output: two extension surfaces frozen and published — `XR_EXT_spatial_workspace` (the compositor-side extension: window pose, focus, capture) and `XR_EXT_app_launcher` (launcher tile registry, click events). Header sketches in [spatial-workspace-extensions-headers-draft.md](spatial-workspace-extensions-headers-draft.md).

### Phase 3 — Severance (separate branches, weeks)

Once Phase 2 is stable and the extensions are frozen, the shell repo can consume the runtime as a published artifact rather than building from source in-tree. After that, the topology becomes:

| Repo | Visibility | Role |
|---|---|---|
| `DisplayXR/displayxr-runtime` | **Public** | Full source. The only runtime repo. |
| `DisplayXR/displayxr-shell-pvt` (new) | Private | Proprietary shell source, consumes published runtime |
| `DisplayXR/displayxr-shell-releases` | Public | Binary shell releases (unchanged) |
| `DisplayXR/displayxr-runtime-pvt` (current) | **Archived** | Historical |

External contributors PR directly against `displayxr-runtime`. The current `apply-public-pr.sh` flow disappears.

## Phase 1 — Detailed boundary-rename map

Each row is a single mechanical edit. Order is bottom-up so dependents land last.

### IPC protocol (`src/xrt/ipc/shared/proto.json`)

The 14 `shell_*` RPCs become two extension namespaces. Workspace lifecycle and window/capture management go under `workspace_`; launcher concerns go under `launcher_`.

| Old | New |
|---|---|
| `shell_activate` | `workspace_activate` |
| `shell_deactivate` | `workspace_deactivate` |
| `shell_get_state` | `workspace_get_state` |
| `shell_set_window_pose` | `workspace_set_window_pose` |
| `shell_get_window_pose` | `workspace_get_window_pose` |
| `shell_set_visibility` | `workspace_set_window_visibility` |
| `shell_add_capture_client` | `workspace_add_capture_client` |
| `shell_remove_capture_client` | `workspace_remove_capture_client` |
| `shell_capture_frame` | `workspace_capture_frame` |
| `shell_set_launcher_visible` | `launcher_set_visible` |
| `shell_clear_launcher_apps` | `launcher_clear_apps` |
| `shell_add_launcher_app` | `launcher_add_app` |
| `shell_poll_launcher_click` | `launcher_poll_click` |
| `shell_set_running_tile_mask` | `launcher_set_running_tile_mask` |

This drives the corresponding rename of `ipc_call_shell_*` and `ipc_handle_shell_*` symbols in the generated IPC code (regenerates from proto.json) plus the 15 handler implementations in `src/xrt/ipc/server/ipc_server_handler.c`.

### Core compositor interface (`src/xrt/include/xrt/xrt_compositor.h`)

| Old | New |
|---|---|
| `bool shell_mode;` (line 2506) | `bool workspace_mode;` |

This field is the single most visible "shell" in the public runtime headers. Renaming it is the point of Phase 1.

### Public functions in `comp_d3d11_service.h` (the API used by IPC handlers)

| Old | New |
|---|---|
| `comp_d3d11_service_ensure_shell_window` | `comp_d3d11_service_ensure_workspace_window` |
| `comp_d3d11_service_deactivate_shell` | `comp_d3d11_service_deactivate_workspace` |
| `comp_d3d11_service_set_launcher_visible` | _unchanged_ (already neutral) |
| `comp_d3d11_service_clear_launcher_apps` | _unchanged_ |
| `comp_d3d11_service_add_launcher_app` | _unchanged_ |
| `comp_d3d11_service_poll_launcher_click` | _unchanged_ |
| `comp_d3d11_service_set_running_tile_mask` | _unchanged_ |
| Doc comments referencing "shell mode" / "in shell mode" | "workspace mode" / "while a workspace is active" |

The launcher functions are already neutrally named — the `shell_*` prefix only ever appeared in the IPC namespace, not the C function names.

### Service orchestrator + config (`src/xrt/targets/service/`)

Today `service_orchestrator.c` hardcodes `displayxr-shell.exe` in two `sibling_exe_path()` calls (lines 235, 257). After Phase 1 this is read from config:

| File | Change |
|---|---|
| `service_config.{c,h}` | Add `char workspace_binary[MAX_PATH]` field (default: `"displayxr-shell.exe"`); add `"workspace_binary"` JSON key alongside the existing `"shell"` enable/auto/disable mode key |
| `service_config.{c,h}` | Rename existing `cfg->shell` (a `service_child_mode` enum) to `cfg->workspace`. Keep the JSON key `"shell"` for backwards-compat *or* migrate with a one-version overlap accepting both. **Recommend**: read both keys, write only `"workspace"`, log a one-time deprecation if `"shell"` is found. |
| `service_orchestrator.c` | Replace hardcoded `"displayxr-shell.exe"` with `s_cfg.workspace_binary`. Rename `s_shell_pi`, `s_shell_running`, `shell_watch_thread_func`, `spawn_shell` → `s_workspace_pi`, `s_workspace_running`, `workspace_watch_thread_func`, `spawn_workspace`. Hotkey ID comment at line 37 ("shared with the shell") becomes "shared with the workspace controller". |
| `service/main.c` | `--shell` CLI flag becomes `--workspace` (keep `--shell` as a deprecated alias for one release). Variable `shell_mode` → `workspace_mode`. |

Once `workspace_binary` is config-driven, an OEM shipping their own workspace controller drops a config file naming their binary — no runtime change needed.

### Env var (`src/xrt/auxiliary/util/u_sandbox.c:126`)

| Old | New |
|---|---|
| `DISPLAYXR_SHELL_SESSION` | `DISPLAYXR_WORKSPACE_SESSION` |

Affects: this file, the shell's `targets/shell/main.c` (which sets the env var on launched apps), CLAUDE.md docs in two places, and any test scripts that reference it.

Recommend: read both, prefer the new name, log deprecation if the old name is set. Drop the alias one release later.

### Documents that lead with "shell" framing

| File | Change |
|---|---|
| `docs/roadmap/shell-runtime-contract.md` | Rename to `docs/roadmap/workspace-runtime-contract.md`. Body: rename `shell_*` → `workspace_*` / `launcher_*` to match the new IPC names. Reframe intro from "the contract between the shell and the runtime" to "the contract between a workspace controller and the runtime — implemented by the DisplayXR Shell as the reference workspace controller, but available to any privileged IPC client". |
| `docs/getting-started/overview.md:68` | Replace _"DisplayXR also includes a service, a spatial shell, and a WebXR bridge"_ with _"DisplayXR also includes a service (for multi-app and sandboxed browsers) and a WebXR bridge. A workspace controller can compose multiple apps in a shared 3D space via XR_DISPLAYXR_spatial_workspace; the DisplayXR Shell is our reference implementation."_ |
| `docs/architecture/production-components.md` | Reframe the "shell" component as "workspace controller (reference: DisplayXR Shell)". 14 mentions to update. |
| `docs/roadmap/spatial-desktop-prd.md` | Internal product vision — leave story intact, but switch architectural references from "the shell" to "the workspace controller". 37 mentions. Stays internal. |
| `CLAUDE.md` (root + worktree CLAUDE.md mirrors) | Update "Spatial Shell" milestone label to "Spatial Workspace Controller (reference: DisplayXR Shell)". Update `DISPLAYXR_SHELL_SESSION` references to the new env var name. |

### Symbols **not** renamed in Phase 1 (deliberately deferred)

These stay as `shell_*` in Phase 1 because they are policy that moves out of the runtime in Phase 2 — renaming them now is throwaway work:

- 217 internal mentions in `compositor/d3d11_service/comp_d3d11_service.cpp`
- `comp_d3d11_window_set_shell_mode_active`, `comp_d3d11_window_set_shell_dp`, `shell_input_event` struct, `SHELL_INPUT_RING_SIZE` constant in `comp_d3d11/comp_d3d11_window.{h,cpp}`
- All comments inside `comp_d3d11_service.cpp` body
- `comp_d3d11_service.cpp` references to `pending_shell_reentry`, `last_shell_render_ns`, `shell_raycast_hit_test`, etc.

These get touched only by Phase 2 (when their containing code moves) or by a final cleanup pass after Phase 2 lands.

## Phase 2 — Mechanism-vs-policy audit

This is the design work that determines whether the workspace extensions can be cleanly factored. Each block of "shell"-coupled compositor code falls into one of three buckets:

### Mechanism (stays in runtime, gets neutral name)

Survives in the runtime under non-shell names. Forms the backing implementation of `XR_DISPLAYXR_spatial_workspace` and `XR_DISPLAYXR_app_launcher`.

- Multi-client atlas composition with per-client virtual window poses (`comp_d3d11_service_set_client_window_pose`, slot management, deferred HWND resize on next frame).
- Spatial raycast hit-test mapping cursor screen position to which window plane was hit (`shell_raycast_hit_test` → `workspace_raycast_hit_test`). Pure geometry; no policy.
- Offscreen capture of the pre-weave atlas to PNG (`comp_d3d11_service_capture_frame`). Already neutrally named.
- 2D Windows.Graphics.Capture client slot management (`comp_d3d11_service_add_capture_client` / remove / set pose). Already largely neutral.
- Per-frame composite + present pipeline (`multi_compositor_render`).
- Input event ring buffer for forwarding mouse/keyboard from the workspace window to the focused app's HWND.
- Window-relative Kooima projection per client.

### Policy (moves to workspace process)

Currently lives in compositor; should be entirely owned by the workspace controller process. The runtime gives it primitives (window pose API, capture API, hit-test API) but doesn't decide *what* to show, *which* tiles to display, or *what* a layout preset means.

- Launcher tile registry (`registered_apps`, push/clear/poll-click/running-tile-mask). The runtime today renders the tiles; in Phase 2 the workspace process renders the launcher into a regular client window, and the runtime just composites it like any other window.
- Layout-preset semantics (`grid`, `immersive`, `carousel` from `comp_d3d11_service_apply_layout_preset`). The runtime today owns the math for each preset; in Phase 2 the workspace process computes per-window poses and pushes them via `workspace_set_window_pose`.
- Right-click context menu logic, double-click maximize/restore, edge-drag resize, title-bar drag — all currently in the compositor's input pipeline. In Phase 2 the workspace controller receives raw input events (or hit-test results) and decides how to interpret them.
- Title-bar chrome rendering (close button, minimize button, app name). The compositor today draws this. In Phase 2 the workspace controller draws chrome into its own client window layer that the compositor composites alongside the app window.
- Eye tracking mode policy (when to switch MANAGED ↔ MANUAL based on focus). Belongs to the workspace controller — it knows what's focused.
- Empty-workspace ESC-handling carve-out, `pending_shell_reentry` state, deactivate-shell/restore-2D-windows lifecycle.

### Gone (delete)

Code that exists only because the runtime tries to handle workspace-specific concerns; deleted outright when policy moves out.

- `service_set_shell_mode` plumbing the shell-mode flag into the window's WndProc so its ESC-close path can distinguish empty-shell from non-shell mode (`comp_d3d11_service.cpp:919`). After Phase 2 the compositor doesn't need to know anything about "empty shell"; it has clients or it doesn't.
- Hardcoded shell title-bar height computations sprinkled through the raycast and rendering paths (`title_bar_h_m` in `shell_raycast_hit_test`). Title bars are workspace-decided.
- `pending_shell_reentry` and the shell-deactivated render-skip logic. Replaced by generic "no clients connected → skip render" check.

## Draft extension surfaces

Sketch only — final shape settled during Phase 2 implementation. Listed here so the rename in Phase 1 picks names that survive.

### `XR_DISPLAYXR_spatial_workspace`

Lifecycle:
- `xrDisplayXREnableSpatialWorkspaceEXT(session, enable)` — privileged client opts into workspace mode. Subsequent `xrLocateViews` etc. behave as workspace-controller (composites multiple client views), not as a regular OpenXR client.
- `xrDisplayXRGetWorkspaceStateEXT(session, out_state)` — query active flag, connected client count.

Window management (each "window" is one connected IPC client):
- `xrDisplayXRSetClientWindowPoseEXT(session, client_id, pose, width_m, height_m)`
- `xrDisplayXRGetClientWindowPoseEXT(session, client_id, out_pose, out_w, out_h)`
- `xrDisplayXRSetClientVisibilityEXT(session, client_id, visible)`
- `xrDisplayXREnumerateClientsEXT(session, count_in, out_count, out_clients)` — enumerate connected app sessions.

Hit-test (raycast cursor → which client window):
- `xrDisplayXRWorkspaceHitTestEXT(session, cursor_screen_pos, out_client_id, out_local_uv)` — pure geometry, runtime-side. The workspace controller calls this from its input handler, then decides whether the hit means focus / drag / right-click forward.

Capture:
- `xrDisplayXRAddCaptureClientEXT(session, hwnd, name, out_client_id)` — start a 2D Windows.Graphics.Capture session as a workspace-resident slot.
- `xrDisplayXRRemoveCaptureClientEXT(session, client_id)`
- `xrDisplayXRCaptureFrameEXT(session, path_prefix, flags, out_result)` — capture pre-weave atlas to PNG. Already a runtime primitive.

### `XR_DISPLAYXR_workspace_tray` (sketch)

The runtime owns the system tray icon — it has to, because the icon exists before any controller is loaded. But the *content* of the workspace submenu (items, labels, handlers) and the *response* to clicks (start, suspend, "Disable", "Lock", whatever the controller chooses to expose) belong to the controller. The current `Enable / Auto / Disable` set and the orchestrator's `TerminateProcess`-on-Disable are first-party-shell defaults that this extension replaces.

- `xrDisplayXRSetWorkspaceTrayMenuEXT(session, menu_descriptor)` — controller registers the tray submenu shape (radio groups, checkboxes, separators, labels, item ids).
- `xrDisplayXRPollWorkspaceTrayClickEXT(session, out_item_id)` — poll-and-clear the most recent menu click. The controller decides what each id means; the runtime just relays.
- `xrDisplayXRRequestControllerShutdownEXT(session)` — runtime asks the controller to exit cooperatively (system shutdown, OEM device-management agent, last-resort-before-kill). Symmetric to a third-party launcher politely asking the controller to leave.

The runtime keeps a last-resort `TerminateProcess` fallback for unresponsive or unregistered controllers, but never calls it on a controller that has registered a menu and is responsive. **See ADR-016** for the architectural rationale and the current transitional state.

### `XR_DISPLAYXR_app_launcher`

Optional second extension. Smaller surface; could even be merged into `spatial_workspace`. Kept separate for now because it's the one most likely to evolve as launcher UX matures.

- `xrDisplayXRClearLauncherAppsEXT(session)`
- `xrDisplayXRAddLauncherAppEXT(session, app_descriptor)` — push one tile (icon + name + opaque id).
- `xrDisplayXRPollLauncherClickEXT(session, out_app_id)` — poll-and-clear most recent click. -1 if none.
- `xrDisplayXRSetLauncherVisibleEXT(session, visible)`
- `xrDisplayXRSetRunningTileMaskEXT(session, mask)` — bitmask of which tiles correspond to currently-running clients (drives glow border in launcher UI).

**Open question for Phase 2**: should the launcher even be a runtime extension at all? Alternative: the workspace controller renders the launcher into a regular client window and uses `spatial_workspace` to position it. That's cleaner architecturally but loses the runtime's ability to render the launcher with no clients connected. The current code path exists because of the empty-workspace startup flow.

## Narrative copy (ships with Phase 1)

Code rename without doc rename leaves a confusing public artifact. Phase 1 commits include these doc updates.

### `DisplayXR/.github/profile/README.md`

Add one-line preamble above the existing repo table:

> DisplayXR is an open OpenXR runtime + extension stack for 3D displays. Build apps, build a workspace controller, or use the reference shell.

Change the `displayxr-shell` row text from _"Spatial shell / 3D window manager"_ to:

> Reference workspace controller (built on the spatial workspace extensions). One example of a privileged compositor client; build your own for verticals, kiosks, OEM workspaces, or AI-agent drivers.

### `displayxr-website` — `components/home/SolutionSection.tsx`

Replace the existing "Spatial Desktop Shell" feature card (line 43) with a new card that frames the extensions as the runtime feature, not the shell:

```ts
{
  title: "Workspace Extensions",
  description:
    "XR_DISPLAYXR_spatial_workspace + XR_DISPLAYXR_app_launcher — extensions that let any privileged client compose multi-app 3D layouts, drive window placement, and register launcher tiles. Build a vertical workspace, an OEM-branded shell, a kiosk, or a test harness.",
  icon: <AppWindow size={20} />,
},
```

Then add a new `<section>` after `SolutionSection` titled **"DisplayXR Shell — our reference workspace"**, one paragraph framing the shell as one possible workspace controller built on these extensions, with a link to `displayxr-shell-releases`.

### `displayxr-website` — `components/home/EcosystemMap.tsx`

Section copy:

> ~~DisplayXR is developing as a full ecosystem — runtime, extensions, engine plugins, projection math, demos, and a spatial desktop shell.~~

becomes:

> DisplayXR is developing as a full ecosystem — runtime, extensions, engine plugins, projection math, demos, and reference workspace controllers.

### `displayxr-website` — `lib/data/ecosystem.ts`

The `displayxr-shell-releases` entry's `description` becomes:

> Reference spatial workspace controller — built on `XR_DISPLAYXR_spatial_workspace`. One example of a privileged compositor client; build your own for verticals, kiosks, or OEM-branded workspaces.

### Runtime — `docs/getting-started/overview.md:68`

> ~~The diagram above shows the in-process path — a single app talking directly to the display. In production, DisplayXR also includes a service (for multi-app and sandboxed browsers), a spatial shell (3D window manager), and a WebXR bridge (metadata sideband for Chrome).~~

becomes:

> The diagram above shows the in-process path — a single app talking directly to the display. In production, DisplayXR also includes a service (for multi-app and sandboxed browsers) and a WebXR bridge (metadata sideband for Chrome). A privileged client can act as a **workspace controller** via `XR_DISPLAYXR_spatial_workspace`, composing multiple apps in a shared 3D space — the DisplayXR Shell is our reference workspace controller.

## Severance checklist (Phase 3 prerequisites)

Severance becomes possible when **all** of these are true:

- [ ] Phase 1 boundary rename complete (this branch)
- [ ] Phase 2 mechanism-vs-policy split complete: launcher registry, layout-preset semantics, chrome rendering, capture-client lifecycle have moved out of the runtime into the shell process
- [ ] `XR_DISPLAYXR_spatial_workspace` and `XR_DISPLAYXR_app_launcher` headers published to `displayxr-extensions` with frozen wire format
- [ ] `service_orchestrator.c` no longer hardcodes `displayxr-shell.exe` (Phase 1 covers this)
- [ ] `targets/shell/CMakeLists.txt` switches from in-tree paths to consuming a published runtime artifact (release zip / git submodule against a tagged runtime version)
- [ ] Shell repo gets its own CI that builds against the latest published runtime, not source-coupled

After all six: the shell repo can move to `DisplayXR/displayxr-shell-pvt`, the runtime repo's `targets/shell/` directory disappears, `displayxr-runtime-pvt` archives, and `apply-public-pr.sh` retires.

**Costs to weigh before committing to severance:**

- Loss of cross-cutting atomic refactors. Today a refactor touching both compositor and shell is one PR; after severance the runtime change must ship in a release before the shell can adopt it. Every ABI-impacting refactor needs a version-overlap design.
- Release-cadence coupling becomes user-visible. Users see "shell vX.Y.Z requires runtime ≥ vA.B.C" instead of trusting that same-SHA = compatible. Already partially the case for demos.
- The two extensions become a hard contract — same discipline as Khronos extensions. Mostly upside, but reduces future flexibility.

## Out of scope for this plan

- macOS workspace controller. The current shell is Windows-only. The extension surfaces should be platform-neutral by design, but the macOS port is its own project.
- WebXR bridge interaction with workspace mode. Phase 2 should ensure WebXR clients can be workspace tiles, but the bridge itself isn't being renamed.
- Adding new workspace primitives (e.g. depth-aware drop shadows, cross-window highlights). The extensions ship with what the current shell needs and grow from there.

## Open questions

1. Should `XR_DISPLAYXR_app_launcher` be a separate extension or merged into `spatial_workspace`? Argument for separate: launcher UX is most likely to evolve. Argument for merged: it's a small surface and "the workspace owns its launcher" is a clean conceptual model.
2. When `service_orchestrator.c` reads `workspace_binary` from config and the binary is missing, current behavior is to log a warning and continue. After Phase 1, should it be an error? (Probably warn-and-continue stays — service mode without a workspace is a valid configuration for `_ipc` apps and WebXR.)
3. Naming nit: `comp_d3d11_window_set_shell_mode_active` exists at the D3D11 *graphics* compositor layer (not the service compositor) — it's used to gate the ESC-close behavior. Does this rename to `_set_workspace_mode_active`, or does it become a more abstract "is_managed_by_external_controller" flag? The flag's actual semantic is closer to the latter; renaming purely to "workspace" perpetuates the conflation. Defer to Phase 2.
