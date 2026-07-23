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
vendor plug-in is active (with a matching ABI), and the display dimensions are valid.

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
`ActiveRuntime` value.

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

## Still stuck?

Grab a bug-report dump and open an issue on the
[runtime repo](https://github.com/DisplayXR/displayxr-runtime/issues):

```
displayxr-cli.exe info > dxr-info.txt
```

Attach `dxr-info.txt` and the newest `%LOCALAPPDATA%\DisplayXR\DisplayXR_*.log`.
