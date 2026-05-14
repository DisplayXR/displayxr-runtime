# Workspace Controller Detection — Conditional Tray + Sidecar Manifest (draft)

**Status:** Draft, 2026-04-28. Sister doc to [spatial-workspace-auth-handshake.md](spatial-workspace-auth-handshake.md). Together these define **Phase 2.0** of the workspace-extensions effort.

Defines how the runtime detects whether a workspace controller is installed, and how the tray UI adapts to show or hide workspace lifecycle controls based on that detection. Resolves the architectural awkwardness of the runtime always having a "Workspace" submenu even when no controller binary is present.

## Why this matters

Today the service tray always shows a Workspace submenu (Enable/Auto/Disable) regardless of whether a workspace controller is actually installed. This made sense when the runtime and shell shipped together — the shell binary was always present. As the platform separates (Phase 3 severs the shell repo from the runtime repo), the runtime needs to ship and run *standalone* — without any workspace controller — and still be a useful product:

| What DisplayXR-without-shell delivers | Status today |
|---|---|
| In-process OpenXR for any 3D-display app | works |
| WebXR for Chrome / sandboxed apps via the bridge | works |
| OpenXR routing for sandboxed apps via the service | works |
| Multi-app spatial composition | requires a workspace controller |
| App launcher, chrome, layouts, capture | requires a workspace controller |

A user who installs only the runtime gets the first three. The tray should reflect that: the Bridge submenu always shows (the bridge ships with the runtime); the Workspace submenu shows only when a controller is detected.

This makes the runtime a real platform — installable on its own, useful on its own, with the shell as one optional product that the platform recognizes when present.

## Detection mechanism

**File-presence check + sidecar manifest, evaluated at service startup.**

At init, the orchestrator:

1. Resolves the workspace controller binary path from `service.json::workspace_binary` (already plumbed in Phase 1; defaults to `"displayxr-shell.exe"` resolved as a sibling of `displayxr-service.exe`).
2. Checks whether the file exists. If not → workspace is unavailable; skip the rest.
3. If the file exists, looks for a sidecar manifest at `<binary path with .exe stripped>.controller.json` in the same directory. The manifest is the workspace controller's self-description.
4. If the manifest exists and parses, extract the display name (and any other published metadata). If the manifest is missing or malformed, log a warning and fall back to `"Workspace Controller"` as the display name — the controller is still considered available since the binary is there.

The result is two pieces of orchestrator state:
- `s_workspace_available` — bool, whether to show the submenu and register hotkeys.
- `s_workspace_display_name` — UTF-8 string, what to label the submenu with.

Both are computed once at init. Live updates (user installs the shell while the service is running) require a service restart for now — `service_orchestrator_apply_config` could re-detect on config change in a later iteration if it becomes a real friction point, but it's not worth the complexity in Phase 2.0.

## Sidecar manifest format

Filename: `<binary>.controller.json` next to the binary. For `displayxr-shell.exe`, the manifest is `displayxr-shell.controller.json` in the same directory. The shell installer drops it at install time.

Schema (additive — extra fields are ignored, missing fields use defaults):

```json
{
  "schema_version": 1,
  "display_name": "DisplayXR Shell",
  "vendor": "Leia Inc.",
  "version": "1.4.0",
  "icon_path": "shell-icon.png"
}
```

| Field | Required | Default | Used for |
|---|---|---|---|
| `schema_version` | yes | — | Forward-compat gate. Currently must be 1. |
| `display_name` | yes | "Workspace Controller" if missing | Tray menu label, log strings ("Launched <display_name>") |
| `vendor` | no | "" | Reserved for future use (about-box, ecosystem registry) |
| `version` | no | "" | Logged at spawn for debugging mismatches; eventually used to gate compat against runtime SPEC_VERSION |
| `icon_path` | no | "" | Tray submenu icon; relative to the manifest. Reserved for future use. |

Pattern matches the existing `.displayxr.json` app manifest schema (commit `acd954548`) — same JSON shape, same additive-growth rule. App manifests describe **launcher tiles**; controller manifests describe **the controller binary itself**. Distinct files, distinct purposes — don't conflate.

## Tray UI shape

**Without a workspace controller installed:**

```
DisplayXR (tray icon)
├── Bridge          ▶
│   ├── Enable
│   ├── ● Auto
│   └── Disable
├── Start on login   ✓
├── ─────────────
└── Quit
```

**With the DisplayXR Shell installed:**

```
DisplayXR (tray icon)
├── DisplayXR Shell  ▶            ← display_name from manifest; falls back to "Workspace Controller"
│   ├── Enable
│   ├── ● Auto
│   └── Disable
├── Bridge           ▶
│   ├── Enable
│   ├── ● Auto
│   └── Disable
├── Start on login   ✓
├── ─────────────
└── Quit
```

**With a third-party workspace controller (e.g., a vertical cockpit) installed:**

```
DisplayXR (tray icon)
├── Surgical Cockpit  ▶            ← whatever display_name the cockpit's manifest declared
│   ├── Enable
│   ├── ● Auto
│   └── Disable
├── Bridge            ▶
…
```

The Bridge submenu always shows because the bridge binary ships with the runtime. The Workspace submenu's presence and label are entirely driven by detection.

## Code changes

Rough sketch — finalised during implementation. Lives in Phase 2.0 alongside the auth handshake.

### `service_orchestrator.h` additions

```c
//! Returns true iff a workspace controller binary was found at the
//! configured path at service init time.
bool
service_orchestrator_is_workspace_available(void);

//! Returns the workspace controller's user-visible display name —
//! either from its sidecar manifest or "Workspace Controller" as
//! a fallback when the manifest is missing/malformed. Empty string
//! if no controller is available.
const char *
service_orchestrator_get_workspace_display_name(void);
```

### `service_orchestrator.c` changes

- New static state: `s_workspace_available`, `s_workspace_display_name[256]`.
- New helper `detect_workspace_controller(const struct service_config *cfg)` — runs at init, populates the static state.
- `service_orchestrator_init`: call `detect_workspace_controller(cfg)` before `apply_workspace_mode(cfg->workspace)`.
- `apply_workspace_mode`: short-circuit when `!s_workspace_available` — log once, don't register hotkey, don't try to spawn.

### New helper file: `service_workspace_manifest.{c,h}`

Small parser that reads `<binary>.controller.json` via cJSON and populates a `struct workspace_manifest { char display_name[256]; char vendor[64]; char version[32]; char icon_path[MAX_PATH]; }`. ~100 lines. Lives in `targets/service/` next to `service_config.{c,h}`.

### `service_tray_win.c` changes

- At menu-build time, call `service_orchestrator_is_workspace_available()`. If false, skip appending the Workspace submenu.
- When appending, use `service_orchestrator_get_workspace_display_name()` for the parent menu-item label instead of the hardcoded `L"Workspace Controller"` from commit `4cf3ba93d`. Convert UTF-8 → wide via `MultiByteToWideChar`.

### Default fallback when no manifest

If the workspace controller binary is `displayxr-shell.exe` and no `.controller.json` is found, the tray label is `"Workspace Controller"` (generic). The DisplayXR Shell installer always drops a manifest, so end-users see "DisplayXR Shell" — but a developer who built the shell from source gets the generic label until they place a manifest. That's fine and lossless.

## Hotkey registration interaction

Today `apply_workspace_mode(SERVICE_CHILD_AUTO)` installs the WH_KEYBOARD_LL hook so Ctrl+Space launches the controller. With detection added:

- `s_workspace_available == false` → never install the hook regardless of mode. Ctrl+Space does nothing system-wide. Logged once at startup as `"workspace controller not installed; Ctrl+Space disabled"`.
- `s_workspace_available == true` → existing logic applies.

This avoids the buggy state where the hook is installed but the spawn target doesn't exist (today's behavior in that edge case prints "Cannot find workspace controller binary 'X' next to service" on every Ctrl+Space, in commit `4cf3ba93d`'s addition).

## Edge cases

**Service running, user installs the shell.** Detected only at next service restart. Live-refresh would be nice but adds file-watching complexity. Document the restart requirement in the shell installer's UX (e.g., installer prompts a service restart at the end). Acceptable for Phase 2.

**Service running, user uninstalls the shell.** Same — detected at next service restart. Until then the tray still shows the submenu, but clicking Enable fails. Mitigation: orchestrator's per-spawn file-presence check (already there in `spawn_workspace`) catches the missing binary and logs an error. User sees no spawn happen, restarts service, submenu disappears.

**Manifest parses but `display_name` is empty.** Treat as missing → fall back to `"Workspace Controller"`.

**Manifest's `display_name` contains weird characters.** Tray uses `MultiByteToWideChar(CP_UTF8, ...)` — handles emoji, diacritics, etc. Length-cap at 64 chars to avoid a workspace-controller name overflowing the tray menu (longer names truncate with ellipsis).

**Two manifests in one directory.** Shouldn't happen — one workspace controller binary per directory. If it does (developer accident), the orchestrator reads only the manifest matching `cfg.workspace_binary`'s basename. The other is silently ignored.

**Binary exists but is the wrong architecture (32-bit on a 64-bit service).** Detection passes (file exists). Spawn fails when the user activates. Treat as a runtime failure, not a detection failure — too rare to warrant a header-parse / PE-architecture check at detect time.

## Future evolution: multi-controller registry — ✅ implemented

Implemented as part of the dedicated-shell-installer work (Phase 2.J). The
final shape diverged from the original sketch in two ways:

1. **Registry, not file directory.** Workspace apps register at
   `HKLM\Software\DisplayXR\WorkspaceControllers\<id>` rather than dropping
   `.controller.json` files. NSIS installers write the registry natively;
   no JSON serialization in the installer; orchestrator uses
   `RegEnumKeyEx` for discovery.
2. **`service.json::workspace_binary` was repurposed**, not retired. It
   now holds the **preferred controller id** (e.g. `"shell"`) for tie-
   breaking when multiple controllers are registered, OR an absolute
   path for dev-mode override (testing freshly-built binaries from
   `_package/` without registering). Empty = pick first registered.

The runtime sidecar manifest (`<binary>.controller.json`) is gone —
`service_workspace_manifest.{c,h}` was deleted. All metadata now lives
in the registry subkey: `Binary`, `DisplayName`, `Vendor`, `Version`,
`UninstallString`.

See `docs/specs/runtime/workspace-controller-registration.md` for the full
contract; `src/xrt/targets/service/service_workspace_registry.{c,h}`
for the enumerator API.

Tray multi-controller chooser (when more than one is registered) is
still future work — single registered controller is the path through
the orchestrator today.

## Migration path

Lands as part of Phase 2.0 alongside the auth handshake. Both are small (~few hundred lines each), share the orchestrator-modification surface, and gate the Phase 2.A–2.H code migrations.

Order within Phase 2.0:

1. **(this doc) Controller detection** — add `service_workspace_manifest.{c,h}`, the orchestrator getters, and the conditional tray UI. Standalone; no auth-handshake dependency. Pure addition; existing behavior is preserved when a manifest exists with `display_name = "Workspace Controller"`.
2. **(auth-handshake doc) Auth handshake** — add `service_orchestrator_get_workspace_pid()`, wire PID check into `ipc_handle_workspace_activate`, replace the literal `"displayxr-shell"` match. Removes the last brand coupling in the runtime.
3. **DisplayXR Shell installer drops the manifest.** One-line installer change in the shell's NSIS / WiX. Until this ships, the shell's detected display name falls back to `"Workspace Controller"` — fine, lossless.

After all three: the runtime ships standalone; the shell adds the workspace submenu and authoritative activation rights; OEMs / verticals / kiosks can ship their own controllers with their own manifests and get exactly the same first-class treatment.

## What this is NOT

- **Not a plugin system.** The runtime doesn't load anything from the controller binary into its own process — the controller is a separate process that talks IPC. The manifest is metadata, not code.
- **Not a workspace controller registry / app store.** The controller binary path is configured (`service.json::workspace_binary`), not auto-discovered yet. A registry is the future-evolution path described above; not part of Phase 2.0.
- **Not a way to disable the bridge submenu.** The bridge ships with the runtime and is conceptually part of "the runtime experience." Bridge lifecycle stays unconditional in the tray.
- **Not coupled to the launcher's app-manifest scanner.** The launcher scans `%LOCALAPPDATA%\DisplayXR\apps\` for app tiles. The orchestrator looks at one specific file next to the controller binary. Different mechanisms because they solve different problems (apps are many, controllers are one).
