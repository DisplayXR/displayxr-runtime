# Stereo camera consent — for app and browser developers

How the DisplayXR runtime decides whether your process may read the display's stereo camera
(`XR_DXR_stereo_camera`, [ADR-043](../adr/ADR-043-stereo-camera-source.md)), what the user sees,
and what you have to do. The normative text is the extension spec,
[§7 *Privacy*](../specs/extensions/XR_DXR_stereo_camera.md#7-privacy); this page is the
walkthrough.

**Why the runtime does this at all.** The camera is the eye tracker's. The tracker opened the
device, so the OS camera permission model and its in-use light never see *you* as a consumer. The
runtime therefore enforces the equivalent itself: per-executable consent, a foreground rule, an
in-use indicator, kill switches, and an unlinkable device id. The vendor plug-in sees none of it.

## 1. The three kinds of client

| You are… | Declare | What gates frames |
|---|---|---|
| **A native app with a window** (a 3D call app) | nothing special — enable the extension | consent (§2) + your window must be visible (§3) |
| **A camera-only process** — a browser's video-capture utility, a capture helper, `displayxr-cli camera` | `XrStereoCameraClientInfoDXR` with `XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR` chained on `XrInstanceCreateInfo` | consent (§2); no window rule (you have none) |
| **A consent-delegating client** — a browser that shows its *own* per-origin camera prompt and in-use indicator | the camera-only declaration **and** a registration of your executable (§4) | your own prompt is the consent; your own visibility rule; never RAW frames |

The camera-only declaration makes the service admit you as the `CAMERA_CONSUMER` class: you may
call only the camera entry points (`xrCreateSession` is refused), and you are **not a panel
owner** — a browser's capture utility no longer spends one of the two present-owner slots, even
though its executable is the browser's. Chain it like this:

```c
XrStereoCameraClientInfoDXR consumer = {
    .type = XR_TYPE_STEREO_CAMERA_CLIENT_INFO_DXR,
    .flags = XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR,
};
XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO, &consumer, /* … XR_DXR_stereo_camera in enabledExtensionNames */};
```

## 2. Consent: what happens at `xrStartStereoCameraStreamDXR`

The service looks at the **OS-derived executable path** of your process (never anything you
send) and walks this list; the first hit decides. The same check runs for
`xrGetStereoCameraCalibrationDXR` (calibration identifies the device).

1. **Sharing off** — the user's tray / menu-bar toggle *"Share the 3D camera with apps"*, or
   `DXR_STEREO_CAMERA=0` in the service's environment → `XR_ERROR_STEREO_CAMERA_DISABLED_DXR`.
   (You also enumerate zero cameras.)
2. `DXR_STEREO_CAMERA_DEV_ALLOW=1` in the **service's** environment → allowed. A development
   override; the service logs one WARN per run. Never on a user's machine.
3. Your executable path could not be verified → `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR`.
4. You are a **registered delegating client** → allowed, no prompt, nothing stored.
5. **Windows:** the OS camera privacy switch denies you (Settings → Privacy → Camera: globally,
   for desktop apps, or for your app) → `XR_ERROR_PERMISSION_INSUFFICIENT`.
6. A **stored decision** for your executable (the user answered before, or an admin pre-seeded
   it) → allowed, or `CONSENT_REFUSED`.
7. The user chose **Allow once** earlier for this very process → allowed.
8. The **prompt**: the service's tray (Windows) / menu-bar item (macOS) shows

   > **"<your application name>" wants to use the 3D camera**
   > *your-app.exe (pid 1234) will receive frames from the display's stereo camera. You can stop
   > it at any time from the DisplayXR menu.*
   > [ Deny ]   [ Allow once ]   [ Allow ]

   **Allow** and **Deny** are remembered for your executable; **Allow once** lasts for this
   process. Your `xrStartStereoCameraStreamDXR` call **blocks** while the dialog is up (up to
   60 s). Unanswered, dismissed, or no prompt available (`DXR_STEREO_CAMERA_PROMPT=0`, a headless
   Linux service) → `CONSENT_REFUSED`, nothing stored — you may try again later.

A refused stream is not destroyed: call start again after the user has allowed you. Map the
results like this in a browser:

| Result | Meaning | `getUserMedia` |
|---|---|---|
| `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` | the user / policy said no | `NotAllowedError` |
| `XR_ERROR_PERMISSION_INSUFFICIENT` | the OS camera switch is off (or wrong client class / RAW asked) | `NotAllowedError` |
| `XR_ERROR_STEREO_CAMERA_DISABLED_DXR` | sharing is off | hide the device, or `NotReadableError` |
| `XR_ERROR_STEREO_CAMERA_BUSY_DXR` | the source cannot be opened right now | `NotReadableError` |
| `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR` | the service ended your stream (see §5) | end the track |

## 3. The foreground rule and the lock screen

If you are a window-bearing client (an ordinary app, or a present-owner), frames are published
only while your process **owns a visible, non-minimised top-level window** (checked every
250 ms). Otherwise your stream is `SUSPENDED`: no new frames, and the frame you had pinned is
cleared so nothing lingers. Camera-only and delegating clients are exempt — the browser's own
tab rule applies to its pages.

Every stream, of every client, is suspended while the **OS session is locked or switched away**
(Windows lock / disconnect, macOS screen lock / fast user switch) and resumes afterwards. You
see it as `XrEventDataStereoCameraStateChangedDXR` with `SUSPENDED` then `AVAILABLE` from
`xrPollEvent` (no session needed), and `xrEnumerateStereoCamerasDXR` reports `SUSPENDED`
meanwhile. Keep showing your last frame or a placeholder; do not tear the stream down.

## 4. Being a consent-delegating client (browsers)

A browser already asks the user per origin and shows its own in-use indicator, so the runtime
should not ask a second time per page. Register the browser's **executable path**:

- **Installer (machine-wide):** Windows `HKLM\Software\DisplayXR\CameraConsent\Delegating`, a
  `REG_SZ` value whose *data* is the full path (value name: anything, e.g. the file name);
  Linux `/etc/displayxr/camera-delegating.json`; macOS
  `/Library/Application Support/DisplayXR/camera-delegating.json` — both
  `{"delegating": ["/full/path/to/browser", …]}`.
- **Per user / development:** `displayxr-cli camera trust <exe>` (or `--self`), which writes the
  user-level list (`HKCU\…\CameraConsent\Delegating`, `~/.config/…/camera_consent.json`).

Obligations that come with it: prompt the user yourself before opening a stream, show your own
in-use indicator, stop the stream when your page stops capturing, and never expose raw
calibration or the raw frame — the runtime enforces the last one (RAW is refused to delegating
clients; `persistentId` is already a per-executable keyed hash, coarsen the rest yourself).

An unsigned / development build that is not registered is simply a camera-only client: the
runtime prompts for it once (Allow is remembered for that path).

### Writing the registration from an installer

The runtime ships no allow-list: **each delegating application's own installer writes its entry
on install and removes it on uninstall.** The runtime's installer never writes or deletes these
entries. Rules, all of which follow from how the runtime checks the entry:

1. **Register the executable the runtime sees.** The runtime identifies a client by the OS-derived
   image path of the process that opens the stream (Windows `QueryFullProcessImageNameW`, Linux
   `/proc/<pid>/exe`, macOS `proc_pidpath`) — never by anything the client sends. In a
   multi-process browser that is the process that calls `xrStartStereoCameraStreamDXR` (typically
   a video-capture utility process), whose image is usually the browser's main executable.
2. **Trust is by full path only** — no signature or hash check in this version. So register only a
   path inside a directory that only an administrator / root can write (Windows `%ProgramFiles%`;
   a root-owned prefix such as `/usr` or `/opt/<vendor>` on Linux). An entry pointing into a
   user-writable directory lets any unelevated process drop an executable there and inherit the
   trust. If the user installs to such a location, register nothing; the runtime then asks once.
   Note that macOS `/Applications` is writable by any admin-group user without elevation.
3. **Protect the entry itself.** `HKLM\Software` is admin-only by default; the POSIX system file
   must be root-owned and not group/world-writable (mode `0644`) — the runtime does not check its
   ownership.
4. **Windows specifics.** Write the **64-bit registry view** (a 32-bit installer such as NSIS needs
   `SetRegView 64`; otherwise the value lands under `WOW6432Node`, which the runtime never reads).
   The value **name** is free: use your product name, so it is unique to you and identifies the
   entry to an admin. Keep the name under 256 characters and the path under 1024 bytes and in
   ASCII — the current reader enumerates with the ANSI registry API and fixed buffers.
5. **POSIX specifics.** The JSON file is shared by every installer: read it, add your path to
   `delegating` if absent, write it back atomically; never rewrite other entries.
6. **Uninstall removes only what you wrote** — your value (Windows) or your path (POSIX), and only
   while it still holds the path you registered. Delete the `Delegating` / `CameraConsent` key or
   the JSON file only if nothing else is left in it.
7. **No restart needed.** The service reads the entry at every stream start and calibration read,
   so a registration or removal takes effect on the next one.
8. **Know what you are asking for.** A delegating client is evaluated at §7.1 step 4, before the
   Windows camera privacy switch (step 5) and before a stored decision (step 6): a user's earlier
   runtime *Deny* for that executable no longer applies, and `displayxr-cli camera untrust` only
   edits the user list, not yours. The user's remaining runtime controls are *"Share the 3D camera
   with apps"* and *"Stop camera sharing"*. Register only software that really prompts per origin.

## 5. What the user can do to you at any time

- **"Stop camera sharing"** (tray / menu bar): every started stream ends. You receive
  `XrEventDataStereoCameraStreamEndedDXR { stream, USER_STOPPED }`, your wake handle fires
  once, and acquire / start on that handle return `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR`.
  Destroy it. You may create and start a new one — that goes through §2 again (and "Allow once"
  grants were forgotten).
- **"Share the 3D camera with apps" off**: same, with reason `DISABLED`; `xrEnumerate…` then
  returns zero cameras until it is switched back on.
- Change their answer: `displayxr-cli camera allow|deny|revoke <exe>` edits the store; the next
  start sees it.

While your stream runs the user sees the runtime's icon badge and *"3D camera in use by
your-app.exe"* in the tray / menu bar — in addition to whatever you show.

## 6. Developing and testing

```sh
# the service, with the sim fake camera; prompts off = a headless CI service
SIM_DISPLAY_FAKE_STEREO_CAMERA=1 DXR_STEREO_CAMERA_PROMPT=0 displayxr-service

displayxr-cli camera probe --seconds 2      # exit 4: CONSENT_REFUSED (no consent, no prompt)
displayxr-cli camera allow --self           # or: camera trust --self (delegating)
displayxr-cli camera probe --seconds 2      # frames
displayxr-cli camera fake-lock on           # every stream SUSPENDED (event printed by probe)
displayxr-cli camera fake-lock off
displayxr-cli camera stop-all               # a running probe prints "ENDED … USER_STOPPED", exits 8
displayxr-cli camera sharing off            # zero cameras; start -> DISABLED (exit 6)
displayxr-cli camera consent                # what is stored, and this CLI's own path
```

`DXR_STEREO_CAMERA_DEV_ALLOW=1` on the service skips consent for everything (loud WARN) — use
it on a dev box when you are iterating on frames, not on the consent path. The full env-var
census is in
[`docs/roadmap/control-panel-performance-settings.md`](../roadmap/control-panel-performance-settings.md).
