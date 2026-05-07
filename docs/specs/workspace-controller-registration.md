# Workspace Controller Registration (Windows)

**Status:** Stable. Implemented in
`src/xrt/targets/service/service_workspace_registry.{c,h}`.

This document is the contract between the DisplayXR runtime and any
**workspace app** that wants to drive the multi-window spatial shell
experience — the DisplayXR Shell, third-party verticals (cockpits,
kiosks, branded shells), or experimental controllers built against
`XR_EXT_spatial_workspace`.

The runtime owns no specific workspace app. There is no
`displayxr-shell.exe` literal in any runtime binary. Workspace apps
**self-register** via the registry contract below; the runtime's
service orchestrator enumerates them at startup and spawns one.

## The contract

A workspace app's installer:

1. **Reads `HKLM\Software\DisplayXR\Runtime\InstallPath` (REG_SZ).**
   If the value is missing or empty, the runtime is not installed —
   the workspace app installer must abort with a clear error message.
   The runtime is a **hard prerequisite** because the workspace app's
   binary lives inside the runtime install tree.
2. **Installs its files into the runtime install directory.** The
   workspace exe (e.g. `displayxr-shell.exe`) sits next to
   `displayxr-service.exe`, `openxr_loader.dll`, and the runtime DLLs
   it binds against. No PATH gymnastics are required.
3. **Writes its registration at `HKLM\Software\DisplayXR\WorkspaceControllers\<id>`.**
   `<id>` is a short unique identifier; vendor-prefix it (e.g.
   `leia-shell`, `lenovo-cockpit`) unless you are DisplayXR's own
   shell. Subkey schema:

   | Value | Type | Required | Purpose |
   |---|---|---|---|
   | `Binary` | REG_SZ | yes | Absolute path to the controller exe. |
   | `DisplayName` | REG_SZ | yes | Tray submenu label, log strings. |
   | `Vendor` | REG_SZ | no | Publisher (reserved for future ecosystem use). |
   | `Version` | REG_SZ | no | Free-form version string (logged at spawn). |
   | `IconPath` | REG_SZ | no | Tray submenu icon (reserved). |
   | `UninstallString` | REG_SZ | yes | Used by the runtime uninstaller for cascade uninstall. **Must honor `/S` (silent).** |

4. **Writes its own Add/Remove Programs entry under
   `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\<ProductId>`.**
   Standard Windows-installer hygiene; lets the user uninstall the
   workspace app from Control Panel without touching the runtime.

The runtime never writes any of these keys.

## Optional: published menu actions

The service tray's workspace submenu (parented under the controller's
`DisplayName`) is rendered from a list of actions the controller
publishes. **The runtime does not hardcode menu items for any
controller.** A controller chooses what items to surface and what each
one does.

Schema — under the controller's `WorkspaceControllers\<id>` key:

```
Actions\
    <ordering>\
        Label = REG_SZ
        Type  = REG_SZ
```

`<ordering>` is the subkey name. The service enumerates via
`RegEnumKeyEx`, which returns subkeys in alphabetical order — use
sortable names like `01-enable`, `02-auto`, `03-disable` to control
menu ordering.

### Supported `Type` values

| Type | Meaning |
|---|---|
| `lifecycle:enable` | Service applies `SERVICE_CHILD_ENABLE` — controller is always running. |
| `lifecycle:auto` | Service applies `SERVICE_CHILD_AUTO` — Ctrl+Space spawns on demand. |
| `lifecycle:disable` | Service applies `SERVICE_CHILD_DISABLE` — controller never auto-spawns. |
| `separator` | Renders an `MF_SEPARATOR`. Not a click target. `Label` is ignored. |
| `controller:<action-name>` | Service runs `CreateProcess` of `Binary` with `--workspace-action <action-name>`. The controller is responsible for singleton-aware forwarding (see below). |

The currently-active lifecycle mode is rendered with `MF_CHECKED` in
the tray submenu.

Unknown `Type` values are skipped silently — forward-compat for
future types like `ipc:<name>` (over the OpenXR session event drain)
or `exec:<cmd>` (raw shell-out without the singleton convention).
Don't invent a new prefix without coordinating with the runtime
maintainers.

### `--workspace-action <name>` command-line contract

When a `controller:<name>` action is clicked, the service runs the
controller binary as if it were:

```
"<Binary>" --workspace-action <name>
```

Controllers MUST honor this convention:

- On invocation with `--workspace-action`, look up an existing
  singleton (e.g., a named mutex). The DisplayXR Shell uses
  `Local\DisplayXR.Shell.Singleton` — third-party controllers should
  use a vendor-namespaced equivalent.
- **If the singleton exists**, forward the action to the running
  instance over a controller-internal channel — `WM_COPYDATA` to a
  known message-only HWND, a named pipe, a file marker, etc. (the
  author's choice). Then exit.
- **If no instance is running**, become the singleton and handle the
  action — typically by spawning the full controller (loading the
  same code paths used in normal startup) and dispatching to the
  newly-loaded handler.

The service does not wait on the spawned process and does not
inspect its exit code. Fire-and-forget.

### Example registrations

**DisplayXR Shell (default install):**

| Subkey | Label | Type |
|---|---|---|
| `Actions\01-enable` | Enable | `lifecycle:enable` |
| `Actions\02-auto` | Auto | `lifecycle:auto` |
| `Actions\03-disable` | Disable | `lifecycle:disable` |

**Hypothetical kiosk-mode controller** (only "Always On" needed):

| Subkey | Label | Type |
|---|---|---|
| `Actions\01-on` | Always On | `lifecycle:enable` |

**Future shell extension** (lifecycle + custom actions):

| Subkey | Label | Type |
|---|---|---|
| `Actions\01-enable` | Enable | `lifecycle:enable` |
| `Actions\02-auto` | Auto | `lifecycle:auto` |
| `Actions\03-disable` | Disable | `lifecycle:disable` |
| `Actions\04-sep` | — | `separator` |
| `Actions\05-launcher` | Show Launcher | `controller:show-launcher` |
| `Actions\06-save` | Save Workspace... | `controller:save-workspace` |

### Fallback

If `Actions\` is **absent** or **empty**, the service falls back to
its hardcoded default workspace submenu — `Enable / Auto / Disable`,
all `lifecycle:*` semantics. This preserves the menu for any
controller that hasn't yet adopted the `Actions` contract.

## Cascade uninstall

When the user uninstalls the runtime via Add/Remove Programs:

1. The runtime's uninstaller enumerates
   `HKLM\Software\DisplayXR\WorkspaceControllers\*`, collecting each
   subkey's `UninstallString` value.
2. For each collected string, it executes `<UninstallString> /S`
   (silent uninstall) **before** removing any runtime files.
3. Each chained workspace app uninstaller removes only the files and
   registry keys it owns — never the runtime's.
4. The runtime then proceeds with its own cleanup.

This pattern mirrors how OS package managers handle dependent
products. It is the workspace app's responsibility to:

- Honor `/S` so the cascade does not block on UI.
- Remove the workspace app's own
  `WorkspaceControllers\<id>` registration key in its uninstall
  section.
- Remove its own Add/Remove Programs entry.
- Remove only the files it installed (its exe, its assets, its own
  uninstaller).
- Never touch shared runtime files (e.g.
  `DisplayXRClient.dll`, `displayxr-service.exe`).

If a workspace app's uninstaller fails or never runs, the runtime
uninstaller's final
`DeleteRegKey HKLM "Software\DisplayXR\WorkspaceControllers"` removes
any orphan registration keys.

## Service-side discovery

At service startup, the orchestrator (`service_orchestrator.c`):

1. If `service.json::workspace_binary` contains a path separator
   (`\` or `/`) → dev-mode absolute-path override; spawn that exact
   binary, skip registry.
2. Else, call `service_workspace_registry_enumerate()` to read all
   `WorkspaceControllers\*` subkeys. Stale entries (whose `Binary`
   does not exist on disk) are skipped silently.
3. If `service.json::workspace_binary` is non-empty, treat it as the
   preferred `<id>`; pick that entry. Fall through to first available
   if not registered.
4. Else, pick the first enumerated entry.
5. If no entries exist, the runtime runs in standalone-platform mode
   — no workspace submenu in the tray, no Ctrl+Space hotkey.

Tray UI (when one or more controllers are registered) shows the
active controller's `DisplayName` as the submenu parent. A future
multi-controller chooser (when more than one is registered) will live
in the tray and toggle `service.json::workspace_binary` between
registered ids.

## Reference implementation

See `installer/DisplayXRShellInstaller.nsi` for the canonical
DisplayXR Shell registration. Third-party workspace apps should mirror
its structure:

- `.onInit` reads the runtime's `InstallPath`, aborts if missing,
  sets `$INSTDIR` to the runtime's install directory.
- Single install section drops the binary, writes a self-uninstaller
  with a distinct name (avoid colliding with `Uninstall.exe`),
  populates the registration key, populates the Add/Remove entry.
- Uninstall section removes only its own files + registry; does not
  touch the runtime's `$INSTDIR` directory itself.

## Versioning

`Version` is informational. The compatibility contract between
runtime and workspace app is enforced by the OpenXR extension layer:
each workspace app declares the minimum runtime `XR_EXT_spatial_workspace`
spec_version it requires (typically in its README; see
`docs/roadmap/demo-distribution.md` for the demo precedent). When
that version is unavailable, the workspace app fails at
`xrCreateInstance` and the user sees a clean OpenXR error rather than
a runtime-crash mystery.

## What this contract is NOT

- **Not a plugin system.** The workspace app runs in its own process
  and talks IPC to the runtime via the public OpenXR extension. The
  runtime does not load any workspace app code.
- **Not a Windows-Service registration.** The runtime's service
  orchestrator (`displayxr-service.exe`) is the parent process; the
  workspace app is just a managed child. No SCM involvement.
- **Not coupled to `service.json`.** That file persists user
  preferences (lifecycle mode, preferred controller id). It is not the
  source of truth for which controllers are installed — the registry
  is.
- **Not a code-signing or trust mechanism.** Any installer with admin
  rights can register a workspace app. If trust gating is needed in
  the future, a separate signature verification step would gate
  enumeration; for now, registration is open.

## Workspace output is opaque

Workspace apps composite client tiles with per-pixel alpha against the
workspace's own background. Each client's projection layer flags
(`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`,
`XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`) are honored at the
per-tile blit so transparent regions of one client tile reveal the
workspace background, not the desktop.

The workspace's **final atlas to the display processor is opaque** —
the workspace itself cannot present a transparent window to the
desktop. Standalone apps that want a transparent output window must
run outside workspace mode (using the in-process D3D11 compositor
with `XR_EXT_win32_window_binding::transparentBackgroundEnabled`).

This is not a temporary limitation; it is a deliberate consequence of
the multi-compositor pipeline. A transparent workspace output would
require punching the chroma-key trick through the combined atlas, which
would let any client paint an effective hole in the desktop — a
trust-boundary regression. Per-tile alpha against an opaque workspace
background covers the realistic use case (transparent window contents
inside a workspace) without that risk.

## Future evolution

If multiple workspace apps need to coexist (one user has the
DisplayXR Shell + a third-party cockpit installed simultaneously):

- Tray submenu grows a controller chooser ("Switch workspace
  controller →") that flips `service.json::workspace_binary` between
  registered ids.
- Runtime restart picks up the new selection on next boot.

Live-switching while the service is running is out of scope; the user
restarts the service via the tray.
