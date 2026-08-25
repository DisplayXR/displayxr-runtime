# Troubleshooting

Field-tested fixes for the problems most likely to bite you when running a DisplayXR
app on real hardware. Each entry is **symptom → cause → fix** — find the symptom you're
seeing and work down.

Every issue here has been reproduced and fixed on real hardware; the fixes are proven,
not speculative.

---

## Step 0 — Always run the self-test first

Before chasing anything else, run the headless self-test. It exercises the real
plug-in-discovery and display-processor path **without a compositor, GPU, or window**, so
it tells you in seconds whether the runtime itself is healthy:

```
"C:\Program Files\DisplayXR\Runtime\displayxr-cli.exe" selftest
```

A healthy run ends with `:: SELF-TEST PASSED` and asserts a display device exists, a
vendor plug-in is active (with a matching ABI), and the display dimensions are valid —
*valid* meaning **equal to the mode the adapter is actually scanning out**
(`EnumDisplaySettingsW`), not merely non-zero. Two checks carry that (#1201):

- `display_dims` fails when the plug-in's pixel size differs from the panel's current
  mode, and names the ratio when it is a round scaling step
  (`reported 2560x1440 px but \\.\DISPLAY1 is 3840x2160 — exactly 150% scaling`).
- `dpi_awareness` fails when the process is not per-monitor DPI aware, because a
  DPI-unaware process is handed virtualised coordinates and none of its geometry can be
  trusted. This one fires even at 100% scaling, where the numbers coincide and
  `display_dims` cannot see the problem.

**The single most useful diagnostic split:** the runtime ships a hardware-free
**sim-display** plug-in alongside the real vendor plug-in. Force each and compare:

```
displayxr-cli.exe dp use sim-display   &&  displayxr-cli.exe selftest   :: no camera/hardware
displayxr-cli.exe dp reset             &&  displayxr-cli.exe selftest   :: real vendor plug-in
```

- **Both pass** → the runtime is fine; your problem is in the app or the compositor path.
- **`sim` passes but the vendor plug-in hangs or fails** → the runtime is fine; the
  problem is **below the runtime** — the vendor SDK, the display hardware, or something in
  your environment (see *App hangs at startup*, below). Do **not** reinstall the runtime;
  it isn't the cause.
- **Both hang/fail** → runtime or install problem (see *Failed to initialize OpenXR*).

`displayxr-cli.exe info` prints a fuller dump for bug reports: runtime version/git tag,
plug-in ABI, the active plug-in's identity and display info, and the Windows
`ActiveRuntime` value. Its `panel mode:` and `DPI aware:` lines are the evidence behind
the `pixels:` line above them — quote all three when reporting a resolution.

On a hybrid iGPU/dGPU machine, `info`'s **`GPU topology`** section (and the matching
one-line `weave placement:` WARN **every** session logs — D3D11, D3D12, the service,
Vulkan and OpenGL alike) tells you whether the weave runs on the adapter that scans out
the panel or has to cross adapters to reach it, and when it does not, a
`split=0 reason=<token>` naming why. The split is ON by default since #918 Phase 3;
`DXR_WEAVE_ON_SCANOUT=0` is the kill switch. See
[Adapter selection](../reference/adapter-selection.md#checking-where-the-weave-actually-runs).

**Which runtime DLL actually loaded?** Every `xrCreateInstance` logs it. Open the newest
log in `%LOCALAPPDATA%\DisplayXR\` (named `DisplayXR_<exe>.<pid>_<timestamp>.log`) and
search for `loaded from:` — that's the authoritative path.

---

## App hangs at startup / freezes right after "Created Leia 3D display"

**Symptom.** An app (or `displayxr-cli selftest` with the real vendor plug-in) launches,
the log reaches the line where the 3D display is created, and then it **hangs forever**
with the process pinned near 0% CPU. `dp use sim-display` + `selftest` **passes**, so the
runtime is healthy.

**Cause — a VPN or endpoint-security tool has injected a Winsock LSP.** Some VPNs and
security suites install a **Winsock Layered Service Provider (LSP)** — a DLL that Windows
loads into *every* process that touches the network and that hooks all socket traffic. The
vendor display SDK opens a small loopback connection to its local service; when that
connection's worker thread is tangled in the LSP's hook, the SDK's context teardown blocks
forever waiting to join the thread. The result is a hard deadlock at session-create that
takes down every DisplayXR app on the machine.

This has been confirmed with **Astrill VPN** (its `ASProxy64.dll` LSP), and the same class
of failure applies to any LSP-injecting product — some enterprise antivirus, Cisco
AnyConnect-style clients, and other "transparent proxy" VPN modes.

Why it's easy to misdiagnose: a plain loopback connection still *connects* (the deadlock is
in thread teardown, not the connect), there's no kernel driver to find, and stopping the
VPN's background service or disconnecting it does **not** remove a DLL that's already
injected.

**Diagnose.** Check the Winsock catalog for third-party providers:

```
netsh winsock show catalog | findstr /i "asproxy proxy lsp"
```

Standard Windows entries are all `MSAFD ...`. Any provider path pointing at a VPN/AV
install directory is a third-party LSP and a prime suspect. To be certain, capture a stack
of the hung process — a thread parked in `_Thrd_join` inside the vendor SDK's context
destructor, with the VPN's proxy DLL also loaded, is the signature.

**Fix.** Remove the LSP and reboot:

```
netsh winsock reset      :: elevated; restores the default Winsock catalog
```

Then **restart the computer** (required — already-running processes keep the injected DLL
until reboot). After the reboot the LSP no longer injects and DisplayXR apps start
normally.

**Keep it from coming back.** The VPN's transparent-proxy mode (e.g. Astrill's "OpenWeb")
re-registers the LSP whenever it's enabled, and some clients re-register it on launch. To
avoid a relapse:

- Use the VPN only in **tunnel modes** (OpenVPN / WireGuard / StealthVPN), which route
  through a virtual network adapter and do **not** install an LSP.
- If you don't need the proxy mode, leave it off, or set the VPN's helper service to
  **manual** start so it can't silently re-register.
- After ever connecting the VPN, spot-check with the `netsh winsock show catalog` command
  above; if the LSP is back, reset + reboot again.

---

## "Failed to initialize OpenXR" / app can't find the runtime / no display found

**Symptom.** The app exits immediately reporting it can't initialize OpenXR, or the runtime
reports `XRT_ERROR_DEVICE_CREATION_FAILED`.

**Causes & fixes:**

- **The active OpenXR runtime points somewhere else.** Windows resolves the runtime from
  `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`. Confirm it points at your DisplayXR
  install (`C:\Program Files\DisplayXR\Runtime\DisplayXR_win64.json`). `displayxr-cli info`
  prints the current value.

- **A SteamVR (or other OpenXR runtime) uninstall blanked the key.** Uninstalling another
  OpenXR runtime can clear `ActiveRuntime` instead of restoring the previous one. Re-point
  it at `DisplayXR_win64.json` — reinstalling DisplayXR, or the installer's repair, restores
  it.

- **No display processor is registered (from-source / dev installs).** On Windows, vendor
  and sim display plug-ins are discovered **only** from the registry
  (`HKLM\Software\DisplayXR\DisplayProcessors`). A packaged install registers one
  automatically; a hand-built runtime does not. If `selftest` reports no active plug-in,
  install the runtime bundle (which registers a display processor), or — for dev builds —
  register one manually (see *Building* → dev-iteration notes).

---

## 3D doesn't weave — the image is flat and the active plug-in is `sim-display`

**Symptom.** Apps launch and `selftest` **passes**, but the picture is flat 2D with no
weaving, and `displayxr-cli info` reports `active plug-in: id=sim-display` even though the
vendor plug-in is installed and its key exists under
`HKLM\Software\DisplayXR\DisplayProcessors`.

**Cause — the vendor SDK isn't reachable on `PATH`, so the plug-in never loads.** The
vendor plug-in DLL links its platform's DLLs, which live in the vendor platform's own
install directory and resolve at load time through the `PATH` entry that the *vendor
platform* installer adds. If they can't be found, `LoadLibrary` fails, the loader logs a
**warning** and falls through to the next plug-in by probe order — the hardware-free
`sim-display`. Nothing reports an error; you just silently get 2D.

Two ways in:

- **The vendor platform was never installed.**
- **The vendor platform was installed *after* DisplayXR.** `PATH` is captured in a
  process's environment block when that process starts. `displayxr-service.exe`
  auto-starts at logon and is long-lived, so it keeps the *pre-install* `PATH` and keeps
  failing the load — even though a freshly-started process would succeed.

**Diagnose.** Open the newest log in `%LOCALAPPDATA%\DisplayXR\` and search for
`plugin loader:`:

```
plugin loader:   <id>: LoadLibrary(...) failed (err=126).
```

`err=126` is `ERROR_MOD_NOT_FOUND` — a *dependency* DLL is missing, i.e. the vendor
platform isn't on `PATH`. (A different error code points elsewhere; an ABI rejection logs
a version mismatch instead, not a load failure.)

**Fix.**

1. Install the vendor platform runtime.
2. Restart the service so it picks up the new `PATH` — or just reboot:

```
taskkill /IM displayxr-service.exe /F
explorer.exe "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"
```

(Relaunch via `explorer.exe` so the service runs **non-elevated**, matching how it starts
at logon.)

3. Re-run `displayxr-cli.exe selftest` and confirm the active plug-in is the vendor one.

You do **not** need to reinstall DisplayXR — discovery is registry-driven at
`xrCreateInstance`, so the plug-in is adopted as soon as its dependencies resolve.

**If the `DisplayProcessors\<vendor>` key is missing entirely,** the plug-in was never
installed: the end-user meta-installer auto-selects the vendor component only when it
detects the vendor platform on disk at startup, and installing the platform later does not
retroactively add it. Re-run `DisplayXRBundle-*.exe` (ARP → *Modify*) and tick the vendor
component, or run its standalone installer — which requires only the DisplayXR runtime.

---

## Vulkan app crashes immediately on launch

**Symptom.** A Vulkan-backed app crashes at or just after startup (often a null-pointer
dereference deep in Vulkan dispatch), while D3D apps run fine.

**Cause.** A stray `vulkan-1.dll` on the system (dropped next to an app, or a second Vulkan
loader on the `PATH`) collides with the real Vulkan loader — two loader images fight over
dispatch and one resolves to null.

**Fix.** Remove the stray `vulkan-1.dll` so only the system Vulkan loader
(`C:\Windows\System32\vulkan-1.dll`) is in play. Check the app's own folder and any
directory you've added to `PATH`.

---

## Panel stuck in 3D after an app crashed

**Symptom.** An app crashed (rather than exiting cleanly) and the panel is still latched in
3D / weaving mode, indefinitely. Every place you would naturally look is clean:

```
> displayxr-cli clients
service: connected, workspace_mode=off, clients=1
id    pid     class          name                             session  io
6     6676    DIAG           displayxr-cli                    ---      y  (self)
```

No app client, `workspace_mode=off`, and `displayxr-cli selftest` passes. The service is
idle and healthy — because it is not the thing holding the panel.

**Cause.** The `SwitchableLensHint` lives in the **SR platform** (`SRService.exe`), not in
the DisplayXR runtime. A clean app teardown releases it; an access violation never does, so
the platform keeps the lens enabled with no owner left to release it. For an **in-process**
app (`_handle` class — every standalone demo) the runtime service was never involved at
all: the app process created the display processor itself, and the only code that could
have released the hint died with the app.

**What does NOT fix it — try neither of these first:**

- **Restarting `displayxr-service` does nothing.** The runtime never held the hint. This is
  the first instinct and it is wrong.
- **Killing orphaned app processes does nothing** — there are none; the app is already gone.

**Fix.** Restart the SR platform services, elevated. Stop `displayxr-service` first so it is
not holding an SR context across the restart, then bring it back:

```bat
taskkill /IM displayxr-service.exe /F
net stop "SR Eye Tracker"  &  net stop "SR Service"
net start "SR Service"     &  net start "SR Eye Tracker"
start "" "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"
```

Bring the service back **non-elevated** (`start ""` from a normal prompt, or via
`explorer.exe`) — it is the on-demand orchestrator for the shell and the WebXR bridge, and
leaving it down or elevated breaks those until next logon.

> If you are measuring anything after this, treat the panel as a fresh baseline: a latched
> lens is exactly the kind of state that makes a later run look inexplicably different.

Tracked as [#1205](https://github.com/DisplayXR/displayxr-runtime/issues/1205).

---

## Eye tracking doesn't work, or the app hangs waiting for tracking

**Symptom.** 3D looks wrong / doesn't follow your head, tracking never engages, or the app
stalls during tracking warm-up. (This is distinct from the VPN hang above — here the SDK
connects but the *tracking hardware* never delivers data.)

**Note the two cameras.** These displays have a dedicated **infrared tracking camera**
(e.g. "SpatialLabs Tracking Camera") that is **separate** from any regular webcam. A
working webcam feed does **not** mean the tracking camera is healthy — check the tracking
camera specifically.

**Causes & fixes:**

- **Cold-boot USB enumeration failure.** The tracking camera intermittently fails to
  enumerate on a cold boot. A **reboot** usually brings it back.
- **Device disabled (Device Manager "Code 22").** If the tracking camera shows a device
  error, open Device Manager, and **disable then re-enable** it (or reboot). This clears
  the disabled state.
- **A Windows update broke the camera pipeline.** A camera/imaging (IPU) driver regression
  from a Windows Update can stop the tracking camera from streaming. Roll back the offending
  update or update the imaging driver.
- **Verify with the vendor's own tools.** The vendor's dashboard/diagnostic app is the
  quickest way to confirm live eye tracking independent of DisplayXR. If it works there but
  not in an app, the tracking hardware is fine and the problem is elsewhere.

---

## Wrong runtime loads, or `XR_RUNTIME_JSON` is ignored

**Symptom.** You set `XR_RUNTIME_JSON` to point at a specific (e.g. dev) runtime, but the
app loads a different one anyway.

**Cause.** The Khronos OpenXR loader **silently ignores `XR_RUNTIME_JSON` in elevated
(administrator) processes** and falls back to the machine-wide
`HKLM\Software\Khronos\OpenXR\1\ActiveRuntime`.

**Fix.** Launch the app from a **non-elevated** terminal, or point the machine's
`ActiveRuntime` at the runtime you want. Confirm which DLL actually loaded via the
`loaded from:` line in the log (see *Step 0*).

---

## Hand tracking reports unsupported even though a provider claimed the roles

**Symptom.** `displayxr-cli selftest` shows the provider claimed the hand-tracking
roles, but in an app `XrSystemHandTrackingPropertiesEXT.supportsHandTracking` is 0
and `xrCreateHandTrackerEXT` fails — and everything else (controllers, actions)
works.

**Cause.** An **implicit OpenXR API layer** is intercepting `XR_EXT_hand_tracking`
before it reaches the runtime. The usual culprit is the Ultraleap Gemini / LeapSDK
install, which registers `XR_APILAYER_ULTRALEAP_hand_tracking`
(`HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` →
`C:\Program Files\Ultraleap\OpenXR\UltraleapHandTracking.json`). The layer
advertises and claims the extension itself, answers the system-properties query
from its *own* device detection (0 with no Leap camera), and never forwards any of
it to DisplayXR — so the runtime's parse never even sees the extension. Note the
selftest does NOT catch this: `displayxr-cli` talks to the runtime directly (no
OpenXR loader, so no layers), which is exactly why the two disagree.

**Fix.** Disable the layer for the process (each implicit layer JSON names its
`disable_environment` variable — for Ultraleap:
`set DISABLE_XR_APILAYER_ULTRALEAP_HAND_TRACKING_1=1`), or machine-wide by setting
the layer's registry value to 1. Keep the layer enabled only if you *want* Leap
hand tracking delivered by Ultraleap's layer instead of a DisplayXR input
provider.

---

## Still stuck?

Grab a bug-report dump and open an issue on the
[runtime repo](https://github.com/DisplayXR/displayxr-runtime/issues):

```
displayxr-cli.exe info > dxr-info.txt
```

Attach `dxr-info.txt` and the newest `%LOCALAPPDATA%\DisplayXR\DisplayXR_*.log`.
