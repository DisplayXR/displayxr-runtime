# Input-Provider Onboarding

This guide takes a hardware vendor from zero to a shipping DisplayXR **input provider**: a plug-in that puts your controllers, stylus, hand tracker or other tracked device in front of every OpenXR app on a DisplayXR display, through the app's ordinary OpenXR actions, with no vendor code in the app and none in the runtime.

It is the input-side counterpart of the [vendor plug-in onboarding guide](vendor-plugin-onboarding.md) (display processors). The two plug-in types are separate on purpose ([ADR-034](../adr/ADR-034-input-provider-plugins.md)): a tracking vendor is not a display vendor, so the contracts version, ship and install independently.

> **Where to start**
> - **`displayxr-input-template`** — the starting point: a buildable, vendor-SDK-free provider skeleton that implements a **stylus** (the profile, the dominant-hand device type, the glass-registered frame, presence, a haptics stub and declared settings), plus a build script, an NSIS installer that registers and allowlists it, and a POSIX manifest. It is fetched against the public runtime headers only. *(Repository not published yet — TODO: link once it is.)*
> - `src/xrt/drivers/sim_input/` (this repo) — the in-tree reference provider for a **controller pair**, hand joints and a navigation device. Opt-in, hardware-free.
> - `src/xrt/drivers/sim_stylus/` (this repo) — the in-tree reference provider for a **stylus**. Opt-in, hardware-free, the thing to compare your provider against.
> - `src/xrt/drivers/ultraleap/` and `src/xrt/drivers/net_input/` — real-hardware and wire-protocol providers: the shape of a process-scoped transport, presence from transport events, and haptics round-trips.
>
> **The normative texts** — this guide explains and orders them; where it and they differ, they win:
> [`xrt_input_plugin.h`](../../src/xrt/include/xrt/xrt_input_plugin.h) (the C ABI),
> [input-provider discovery spec](../specs/runtime/input-provider-discovery.md) (discovery, roles, frames, the rig),
> [`xrt_input_plugin_iface` reference](../reference/xrt_input_plugin_iface.md) (the vtable and device obligations),
> [ADR-034](../adr/ADR-034-input-provider-plugins.md) and its amendments (the why).

## Contents

1. [What you ship](#1-what-you-ship)
2. [The contract in one page](#2-the-contract-in-one-page)
3. [ABI and versioning](#3-abi-and-versioning)
4. [Devices, roles and interaction profiles](#4-devices-roles-and-interaction-profiles)
5. [A stylus: the MX Ink profile](#5-a-stylus-the-mx-ink-profile)
6. [Frames: where your poses live](#6-frames-where-your-poses-live)
7. [The rig rule](#7-the-rig-rule)
8. [Poses and time](#8-poses-and-time)
9. [Haptics](#9-haptics)
10. [Presence and hot-plug](#10-presence-and-hot-plug)
11. [Settings and handedness](#11-settings-and-handedness)
12. [Discovery, per OS](#12-discovery-per-os)
13. [Provenance: sign it, or allowlist it](#13-provenance-sign-it-or-allowlist-it)
14. [The installer](#14-the-installer)
15. [Testing](#15-testing)
16. [How far your device reaches: native apps and engines](#16-how-far-your-device-reaches-native-apps-and-engines)
17. [Checklist](#17-checklist)

## 1. What you ship

1. **A provider binary** — `DisplayXR-<YourDevice>.dll` (Windows), `.dylib` (macOS), `.so` (Linux), or `libdxri<NNN>_<id>.so` packaged into the runtime APK (Android, §12). It exports exactly one C symbol, `xrtInputPluginNegotiate`, and owns your entire tracking stack: transport (USB, radio, Bluetooth, network), sensor fusion, threads, your SDK if you have one.
2. **A registration** — a Windows registry row under `HKLM\Software\DisplayXR\InputProviders\<id>`, or a POSIX JSON manifest `NNN-<id>-input-provider.json`, written by your own installer.
3. **A provenance** — an Authenticode signature on the binary, or, for an unsigned build, an allowlist row for its exact SHA-256 (§13).

What you do **not** ship: anything in the runtime's install directory, a head device, an OpenXR API layer, or app-side code. The runtime turns your devices into OpenXR actions for every app; the app never knows which vendor is behind them.

## 2. The contract in one page

```c
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_device.h"

XRT_INPUT_PLUGIN_EXPORT xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version);
```

| Step | Who | What happens |
|---|---|---|
| Negotiate | runtime → you | You keep `host` (process-lifetime storage), fill a static `xrt_input_plugin_iface` (`struct_size = sizeof(...)`, `id`, `display_name`, `vendor`, `version`, the callbacks), return it and `XRT_INPUT_PLUGIN_API_VERSION_CURRENT`. |
| `probe(&inst)` | runtime → you | "Should I be loaded on this system?" Cheap — sub-millisecond, on the `xrCreateInstance` path. `XRT_SUCCESS` keeps you resident; `XRT_ERROR_PROBER_NOT_SUPPORTED` is a clean decline. Every provider that probes successfully stays resident for the process (ADR-034 Amendment 3). |
| `create_devices(inst, out, max, &n)` | runtime → you | Return up to `XRT_INPUT_PLUGIN_MAX_DEVICES` (8) `xrt_device`s. Called **once per `xrCreateInstance`**, not once per process — keep expensive machinery process-scoped (§10). Ownership of the devices passes to the runtime, which destroys them through `xrt_device::destroy`. |
| `get_presence(inst)` | runtime → you | "Is your hardware attached **right now**?" Re-asked for the life of the process; decides whether you or the fallback hold the roles (§10). |
| `describe_settings(&descs)` | runtime → you | The keys you read through `host->get_setting` (§11). |
| `destroy(inst)` | runtime → you | Free provider state. Today the runtime never unloads a provider; do not rely on this being called. |
| Device callbacks | runtime → your devices | `update_inputs`, `get_tracked_pose`, `set_output`, `destroy` — on the `xrSyncActions` / `xrLocateSpace` / `xrApplyHapticFeedback` paths. |
| Host callbacks | you → runtime | `get_display_geometry`, `get_setting`, `get_handedness`, `get_camera_pose`, the shared-camera slots, `poll_pointer_events`. All optional; NULL-check every one. |

Your binary is loaded **in-process**: inside `displayxr-service.exe` for service-composited apps, and inside the app for every in-process mode. A provider that blocks, crashes or calls `exit()` takes its host down with it. Never block a runtime callback; do I/O on your own threads.

## 3. ABI and versioning

The input-provider ABI follows [ADR-020](../adr/ADR-020-plugin-abi-compatibility-policy.md), independently of the display-processor ABI.

- **One major: `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` = 1.** Return exactly the value from the header you compiled against. A different major is rejected by the loader before any vtable call; you are skipped and the next provider (or the keyboard/mouse fallback) takes the roles.
- **Your vtable carries `struct_size`.** The runtime reads only what your `struct_size` covers and guards every appended slot with `XRT_INPUT_PLUGIN_IFACE_HAS`. A provider built against an older header keeps loading on a newer runtime.
- **The host iface carries `struct_size` too.** Never read a host field at or past `host->struct_size`, never dereference `reserved[]`, and NULL-check every callback — an older runtime leaves newer slots NULL. New host slots are carved out of `reserved[]` at an unchanged size, which is why they do not bump the major.
- **Feature macros tell you what your header has.** Guard optional code with them so one source tree builds against several runtime pins:

  | Macro | Means the header carries |
  |---|---|
  | `XRT_INPUT_PLUGIN_HOST_HAS_GET_SETTING` | `host->get_setting` (the options contract) |
  | `XRT_INPUT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS` | your `describe_settings` slot |
  | `XRT_INPUT_PLUGIN_HOST_HAS_POLL_POINTER_EVENTS` | `host->poll_pointer_events` |
  | `XRT_INPUT_PLUGIN_HOST_HAS_GET_CAMERA_POSE` | `host->get_camera_pose` |
  | `XRT_INPUT_PLUGIN_HOST_HAS_SHARED_CAMERA` | the four shared-camera slots |
  | `XRT_INPUT_PLUGIN_HOST_HAS_GET_HANDEDNESS` | `host->get_handedness`, and the runtime resolves `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` |
  | `XRT_INPUT_PLUGIN_HAS_STYLUS` | the stylus pieces: `XRT_DEVICE_MX_INK_STYLUS`, the `XRT_INPUT_MX_INK_*` inputs, `XRT_OUTPUT_NAME_MX_INK_HAPTIC`, `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER`, `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` |

  The macro tells you what you **compiled** against. What the **running** runtime supports is told by the slot being non-NULL inside `host->struct_size` (for host slots) — check both.

- **The devices you return are not covered by input ABI 1.** `struct xrt_device`, the `xrt_input` / `xrt_output` arrays and `xrt_tracking_origin` have no `struct_size` and are read by the runtime directly, and the enums you fill them with are numbered by position. In practice that means: **build against the runtime release you target, and rebuild when you move to a newer one.** Do not ship one binary against a range of runtime versions on the strength of the major alone. Hardening this is [ADR-063](../adr/ADR-063-input-provider-device-abi-hardening.md) (Proposed).

The stylus pieces (`XRT_INPUT_PLUGIN_HAS_STYLUS`) arrive with ADR-034 Amendment 8. A provider that must also load on an older runtime cannot offer the stylus profile there; see §5 *Fallback profile*.

## 4. Devices, roles and interaction profiles

Every device you return is an ordinary `xrt_device` that describes itself. Two fields decide what the runtime does with it:

**`device_type` decides the role it competes for.**

| `device_type` | Role | Use it for |
|---|---|---|
| `XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER` / `_RIGHT_HAND_CONTROLLER` | that hand | A controller pair whose sides are fixed. |
| `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` | the user's dominant hand | **A single hand-held tool — a stylus.** The builder rewrites it in place to LEFT or RIGHT from the user's handedness policy (§11) before any role, IPC export or CLI listing sees it. |
| `XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER` | left first, then right | A pair whose sides are interchangeable. **Never a single pen** — it always lands on the left hand first. |
| `XRT_DEVICE_TYPE_NAVIGATION` | the rig | A device that moves the user's viewpoint (one per provider; [discovery spec §4b](../specs/runtime/input-provider-discovery.md)). |
| `XRT_DEVICE_TYPE_GAMEPAD` | `/user/gamepad` | A gamepad ([§4c](../specs/runtime/input-provider-discovery.md)). |
| Hand-tracking devices (`supported.hand_tracking`, `XRT_INPUT_HT_*`) | the hand-tracking roles | Optical hands ([`ultraleap/`](../../src/xrt/drivers/ultraleap/), `sim_input`). |
| anything else | none | Rides along; reachable only through the profile it binds. |

Roles are arbitrated by **presence** and **priority** (your `ProbeOrder`, a user's `PreferredPlugin`, a per-app pin): while your hardware is present and you rank highest, you hold the role; when you go absent, the next present provider — and finally the runtime's keyboard/mouse emulated pair — takes it, mid-session, with no app restart (discovery spec §4).

**`name` (plus optional `binding_profiles`) decides the OpenXR interaction profile it serves.** The runtime ships a compiled-in table of profiles (`src/xrt/auxiliary/bindings/bindings.json`, 30+ of them: `khr/simple_controller`, the common VR controllers, gamepads, and the stylus profile below). You pick an existing profile by setting `xdev->name` to its device name and filling `inputs[]` / `outputs[]` with that profile's `XRT_INPUT_*` / `XRT_OUTPUT_NAME_*` names. The app suggests bindings for the same profile and the runtime connects the two. You may fill any subset of a profile's inputs; paths with no backing input simply never change.

You cannot add a profile from a plug-in — profiles are part of the runtime's OpenXR surface. If no existing profile fits your hardware, open an issue on this repo; the answer is usually an existing public profile (as for the stylus) or a new `XR_DXR_*` one.

## 5. A stylus: the MX Ink profile

A 6-DoF stylus on a 3D display needs an analog tip, pen buttons, a tip pose and haptics, and its tip must land exactly where it touches the glass. The runtime serves this through the public Khronos vendor extension **`XR_LOGITECH_mx_ink_stylus_interaction`** — the existing, standardised shape for a tracked pen. Any vendor's stylus can serve it; it is a profile, not a product requirement.

| | |
|---|---|
| Extension | `XR_LOGITECH_mx_ink_stylus_interaction` (advertised by default; CMake `XRT_FEATURE_OPENXR_INTERACTION_LOGITECH_MX_INK`) |
| Profile | `/interaction_profiles/logitech/mx_ink_stylus_logitech` on `/user/hand/left` and `/user/hand/right` |
| `xdev->name` | `XRT_DEVICE_MX_INK_STYLUS` |
| `xdev->device_type` | `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` |
| Tracking origin | `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` (§6) |

| OpenXR path (under `/user/hand/<side>`) | `xrt_input` / `xrt_output` name | Type |
|---|---|---|
| `/input/tip_logitech/force` | `XRT_INPUT_MX_INK_TIP_FORCE` | `VEC1_ZERO_TO_ONE` |
| `/input/cluster_middle_logitech/force` | `XRT_INPUT_MX_INK_CLUSTER_MIDDLE_FORCE` | `VEC1_ZERO_TO_ONE` |
| `/input/cluster_front_logitech/click` | `XRT_INPUT_MX_INK_CLUSTER_FRONT_CLICK` | `BOOLEAN` |
| `/input/cluster_front_logitech/double_tap_logitech` | `XRT_INPUT_MX_INK_CLUSTER_FRONT_DOUBLE_TAP` | `BOOLEAN` |
| `/input/cluster_back_logitech/click` | `XRT_INPUT_MX_INK_CLUSTER_BACK_CLICK` | `BOOLEAN` |
| `/input/cluster_back_logitech/double_tap_logitech` | `XRT_INPUT_MX_INK_CLUSTER_BACK_DOUBLE_TAP` | `BOOLEAN` |
| `/input/dock_logitech/docked_logitech` | `XRT_INPUT_MX_INK_DOCKED` | `BOOLEAN` |
| `/input/system/click` | `XRT_INPUT_MX_INK_SYSTEM_CLICK` | `BOOLEAN` |
| `/input/grip/pose` | `XRT_INPUT_MX_INK_GRIP_POSE` | `POSE` |
| `/input/aim/pose` | `XRT_INPUT_MX_INK_AIM_POSE` | `POSE` |
| `/input/tip_logitech/pose` | `XRT_INPUT_MX_INK_TIP_POSE` | `POSE` |
| `/output/haptic` | `XRT_OUTPUT_NAME_MX_INK_HAPTIC` | vibration |

Rules that come from the extension and from the frame:

- **`TIP_POSE`'s frame:** origin at the sharpest point of the tip, −Z pointing out of the tip along the pen's axis, +Y along the input cluster's normal. Get this right and an app's ink lands under the nib.
- **Tip force is pressure, 0 to 1**, with 0 meaning "not touching". Map your sensor's curve here; do not threshold it into a click.
- **Double-tap inputs are short `true` levels** you raise when your firmware detects a double tap. Keep each one up for at least one `xrSyncActions` interval (≈ 2 frames), or a fast app can miss it.
- **`DOCKED`:** report `true` just before the pen stops tracking (it went into its dock or charger). Docked is still **present** (§10) — the hardware is attached; it just has nothing to say.
- **Only the inputs you have.** A pen without a middle force sensor leaves that input out; the path never changes.
- **The app must enable the extension.** Without `XR_LOGITECH_mx_ink_stylus_interaction` enabled, suggesting the profile returns `XR_ERROR_PATH_UNSUPPORTED` — the app's problem to fix, not yours.

**Fallback profile.** An app that knows nothing about styluses still suggests `khr/simple_controller`. Add a `binding_profiles` entry mapping your tip force to `select/click` and your tip/aim pose to `aim/pose` if you want such apps to work. The same entry is what serves an older runtime that lacks the stylus profile (§3).

The template's provider and the reference doc's [*A stylus, end to end*](../reference/xrt_input_plugin_iface.md#a-stylus-end-to-end) show the device setup in code.

## 6. Frames: where your poses live

Your devices share an `xrt_tracking_origin`; its `type` tells the runtime what frame your poses are in. Pick deliberately — it decides where your device lands in every app.

| `tracking_origin->type` | Your poses are | The runtime | Use it for |
|---|---|---|---|
| `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL` | **physical display-plane metres**: origin at the display centre, +X right, +Y up, +Z toward the viewer | anchors the origin at the initial rig (`world = rig(t) ∘ L`); in a session that owns its canvas, places `L` as the **physical point of the glass** the app renders there | **a stylus**, any tool whose contact point with the panel must be honest |
| `XRT_TRACKING_TYPE_RIG_LOCAL` | the same frame | the same anchoring; in a session that owns its canvas, rescales `L` with the canvas (fractions of the display height, ADR-034 Amendment 7) | **hands and controllers** sized to the scene; anything from a provider that also navigates (required there) |
| `XRT_TRACKING_TYPE_OTHER` / `_NONE` | your own stage frame | stage-anchored, mount offsets applied | legacy room-scale trackers. `_NONE` also gets per-hand arm-model offsets injected — never use it for a 6-DoF device |

What `RIG_LOCAL_PHYSICAL` buys a stylus, per session:

| Session | A pose `L` on the glass lands |
|---|---|
| No view rig chained (a runtime-owned window, or an app-owned window that chains no `XR_DXR_view_rig`) | on the physical display plane, unscaled |
| An app-owned window (`_handle`, `_texture`, engine plug-ins) with a display rig (`XR_DXR_view_rig`) | where the app renders that point of the glass: re-based into the canvas like every eye, then the rig's glass scale |
| A camera rig | the same, against the rig's convergence plane |

So a nib touching the panel is registered **exactly**, under every rig. Off the glass it is exact when the display rig's `perspectiveFactor` is 1. Math and per-session details: [discovery spec §4b.4a](../specs/runtime/input-provider-discovery.md).

Publish **your honest volume pose** in that frame. You need the panel's size to build it — `host->get_display_geometry(&geo)` gives panel width and height and the *nominal* viewer position, from your `create_devices` call on (earlier calls return `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY`). If your tracker measures relative to a camera mounted on the display, compose your calibrated camera-to-panel transform yourself, or use `host->get_camera_pose` when your tracking uses the display's own camera.

A provider that publishes both a pen and hands uses **two origins**: hand joints from a `RIG_LOCAL_PHYSICAL` origin keep `RIG_LOCAL` behaviour by design.

## 7. The rig rule

The **rig** is the user's virtual viewpoint — moved by the runtime's WASD / mouse-look fly camera, or by a provider's navigation device. Because `RIG_LOCAL*` origins are anchored at the rig, your devices are **screen-anchored**: when the rig moves, the panel moves through the virtual world and your stylus moves with it, exactly as a pen held against a physical screen would.

The one rule: **never compose navigation, a camera transform, or any rig pre-compensation into the poses you publish.** The runtime applies the rig to every provider device; a pre-compensated pose subtracts a transform the runtime is about to add, and the failure shows up as slow drift, not as an error ([ADR-034 Amendment 2](../adr/ADR-034-input-provider-plugins.md)).

Two corollaries:

- **Eye tracking never moves your device.** Your poses are not parallaxed — the viewer's eyes are the display processor's business, and they enter at the view level only. A pen held still stays still while the user moves their head.
- **Rig motion is not yours to undo.** If the user flies while holding the pen to the glass, the pen stays on the glass.

If your device also drives the viewpoint (a puck you push around the desk, say), that is a separate navigation device with its own contract: [vendor guide §8 *Adding navigation*](vendor-plugin-onboarding.md#adding-navigation) and [discovery spec §4b](../specs/runtime/input-provider-discovery.md).

## 8. Poses and time

- **`get_tracked_pose(name, t)` answers for the requested time `t`**, not "the latest sample". Apps ask for their predicted display time; a stylus served late lags visibly behind the nib.
- **The recommended shape for asynchronously fed hardware:** your transport thread pushes timestamped samples into `m_relation_history` (`src/xrt/auxiliary/math/m_relation_history.h`), and the callback interpolates or predicts for `t` (`m_predict`; `m_filter_one_euro` for jitter). The template links no runtime library, so it ships a two-sample linear extrapolation in its place; link the runtime's aux math library when you need the real one.
- **Clock:** the monotonic clock (`os_monotonic_get_ns` domain). Convert your device timestamps once, on arrival.
- **One latched publication per timestamp.** Serve grip, aim and tip for one `t` from the same sample set; the runtime guarantees nothing across separate calls, and an app reading tip and aim in one frame must see one pen.
- **Relation flags are honest.** Set `POSITION_VALID | ORIENTATION_VALID` (and the `_TRACKED` bits while the optics see the pen) when you have a pose; clear them when you do not. Losing sight of the pen is *invalid poses*, not *absent hardware*.
- **`update_inputs` refreshes buttons and forces** from your latest snapshot; it runs inside `xrSyncActions`, so it must not block.

## 9. Haptics

- `set_output(xdev, XRT_OUTPUT_NAME_MX_INK_HAPTIC, value)` is called from `xrApplyHapticFeedback` / `xrStopHapticFeedback`. `value->type` is `XRT_OUTPUT_VALUE_TYPE_VIBRATION`; `value->vibration` carries `amplitude` (0–1), `frequency` (Hz, 0 = your device's default) and `duration_ns` (the app's `XrHapticVibration::duration` passed through unchanged, so `XR_MIN_HAPTIC_DURATION` = -1 arrives as -1 and means "your shortest pulse"). An amplitude of 0 is a stop.
- **Never block.** Queue the event to your transport thread and return. The call can arrive on the app's thread or, under the service, on an IPC thread.
- **No haptics hardware?** Still declare the output and make `set_output` a no-op returning `XRT_SUCCESS`, so apps that pulse it do not see an error.
- `displayxr-cli input haptic-test [seconds]` fires vibrations at whatever holds the hand roles — the end-to-end check that your output is wired (§15).

## 10. Presence and hot-plug

Two different questions, two different callbacks:

| | `probe()` | `get_presence()` |
|---|---|---|
| Asks | "should I be loaded on this system?" | "is my hardware attached right now?" |
| When | once per process, at the first `xrCreateInstance` | continuously, from the role arbiter on the `xrSyncActions` path |
| Budget | sub-millisecond, no blocking discovery | non-blocking, allocation-free — return a value your transport thread maintains |

- **Probe succeeds whenever your provider is installed and enabled**, even with the pen switched off or the dongle unplugged. The provider set is fixed per process: a provider that declined at probe cannot join later, so declining because the hardware happened to be asleep makes it unreachable until the next service start. Decline only when your provider should never run on this machine (an opt-in reference provider, a platform you do not support).
- **`get_presence` is transport**, not visibility: `PRESENT` while the pen is paired and talking, even when docked or out of the camera's view; `ABSENT` when the dongle is unplugged, the pen is powered off, or the link is lost. Reporting `ABSENT` for a pen that merely left the tracking volume hands the role to the fallback and back on every exit and entry — the user sees the cursor jump.
- **`UNKNOWN`** means "not yet" (still starting up) and is treated as absent. If you can never tell, leave the slot NULL: that means "always present".
- **Plug and unplug just work.** When presence flips, the runtime moves the role to the next candidate and bumps the role `generation_id`; the app's next `xrSyncActions` rebinds and it receives `XrEventDataInteractionProfileChanged`. No restart.
- **Keep expensive machinery process-scoped.** `create_devices` runs once per `xrCreateInstance` — a conformance run calls it hundreds of times. Open your SDK connection or transport once, lazily, on the first call, and let the device objects be the cheap per-instance part (discovery spec §3 *Lifetime*).

## 11. Settings and handedness

**Read your knobs only through `host->get_setting(iface->id, "KEY", buf, cap)`** — never `getenv`. Inside the service the environment is the service's, not the user's, and only the host path is reachable by the CLI. The runtime resolves, highest first: `DXR_INPUT_<ID>_<KEY>` in the hosting process's environment → the per-user store → your machine row's `Settings\<KEY>` (Windows) or the manifest's `settings` object (POSIX) → your compiled default. Values are strings; you parse them. Read at `probe` / `create_devices`, not per frame.

Declare every key in `describe_settings` (a static array of `{key, type, default, help}`) so users and support can see them:

```powershell
displayxr-cli input options <id>                 # every key, its effective value and its source
displayxr-cli input set <id> TIP_THRESHOLD 0.05  # per user, no admin (next session)
displayxr-cli input unset <id> TIP_THRESHOLD
```

**Handedness is runtime policy, not your setting.** The user's dominant hand lives in the input class's policy — Windows `HKCU\Software\DisplayXR\InputProviders\Policy\Handedness` (DWORD, 0 = right, 1 = left; HKLM is the machine default; HKCU is ignored under `Policy\Lock` = 1), POSIX a `handedness` file in the per-user manifest directory (first byte `l` or `L` = left), right when absent. Android has no per-user store yet: there the dominant hand is always the right. Declare your stylus `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER` and the runtime applies it; one policy then serves every vendor's pen. `host->get_handedness()` lets you read it if your firmware needs to know which hand holds it (grip-pose offsets, for example).

On a runtime without `get_handedness` (the slot is NULL), `DOMINANT` claims no role. A provider that must run there too declares `LEFT` or `RIGHT` itself from its own setting — the template's `HAND` key (`auto` / `left` / `right`) does exactly that, and on a newer runtime `auto` defers to the policy.

## 12. Discovery, per OS

**Windows — registry.** `HKLM\Software\DisplayXR\InputProviders\<id>` (64-bit view):

| Value | Type | |
|---|---|---|
| `Binary` | REG_SZ | absolute path of your DLL — required |
| `DisplayName`, `Vendor`, `Version` | REG_SZ | optional, shown by `input list` |
| `ProbeOrder` | DWORD | arbitration priority, lower wins; absent = 100 |
| `Enabled` | DWORD | optional; 0 = skip (users override per user with `input enable|disable`) |
| `Settings\<KEY>` | REG_SZ / DWORD | optional machine defaults for your settings |

Name the subkey after your `iface->id`. Pick a `ProbeOrder` in 1–99 for real hardware and distinct from every provider you expect to coexist with: the in-tree Ultraleap provider ships at 50, the gamepad at 150, `sim_input` at 200. Equal values are broken by id and logged as a warning.

**macOS / Linux — JSON manifest.** A file `NNN-<id>-input-provider.json` (NNN = ProbeOrder, zero-padded) in a plug-in discovery root — per user `~/Library/Application Support/DisplayXR/DisplayProcessors/` (macOS) or `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/` (Linux), system roots per [plugin-discovery.md §3.1](../specs/runtime/plugin-discovery.md). The `-input-provider.json` suffix routes it to the input loader; display processors share the directory.

```json
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "example-stylus",
        "display_name": "Example Stylus",
        "vendor":       "Example Vendor",
        "version":      "1.0.0",
        "binary_path":  "/opt/example-stylus/lib/DisplayXR-ExampleStylus.so",
        "probe_order":  60,
        "settings":     { "HAND": "auto" }
    }
}
```

On ELF export exactly one symbol (a version script like `src/xrt/drivers/input_plugin_exports.version`) and link with `--no-undefined`, so a missing symbol fails your build rather than the user's `dlopen`.

**Android.** No manifest: a provider ships inside the runtime APK as `libdxri<NNN>_<id>.so` in its native lib dir, and settings come from `debug.xrt.DXR_INPUT_<ID>_<KEY>` system properties ([discovery spec §3 *Android*](../specs/runtime/input-provider-discovery.md), [ADR-038](../adr/ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md)). Talk to the runtime maintainers before planning an Android provider.

## 13. Provenance: sign it, or allowlist it

On Windows the loader **refuses unverified binaries** ([ADR-054](../adr/ADR-054-component-model.md) N4b): a provider DLL must be Authenticode-signed (any trusted root) or have its SHA-256 under `HKLM\Software\DisplayXR\Trust\Allowlist` (value name = lowercase hex digest, data = a free-text note). A refused provider logs `REFUSING unverified binary` and is skipped — your users get the keyboard fallback, not your pen. A path is never a provenance: living under `Program Files` proves nothing.

- **Signed releases need nothing else.** That is the expected shape of a shipping provider.
- **An installer that can ship unsigned bytes** (CI artifacts, developer builds) writes the allowlist row for the exact file it installs — digest computed at packaging time, never by hand — and its uninstaller deletes that row and only that row. The template's installer generates both from the packed DLL, following the runtime's own `installer/gen_allowlist_nsh.cmake`.
- **On a development box,** `scripts\register_dev_plugin.bat input <dll> [id]` (elevated) registers *and* allowlists a build-tree DLL; re-run it after every rebuild, since the digest changes. `displayxr-cli input list --json` reports each provider's `provenance` and the reason.
- **POSIX** audits but does not refuse unless `/etc/displayxr/trust/enforce` exists; a root-owned `.deb` install path counts as platform-trusted.

Contract text: [plugin-discovery.md §4.3](../specs/runtime/plugin-discovery.md).

## 14. The installer

The rules are the display-processor installer's ([vendor guide §5](vendor-plugin-onboarding.md#5-installer-contract)), with the input root:

- **You own your row.** Your installer writes `InputProviders\<id>` (and your allowlist row); your uninstaller deletes exactly those. Nothing else ever touches them, and you touch nothing else — not other providers' rows, not `Policy`, not the runtime's files.
- **No install order.** Your installer must not require the runtime: install your files under `C:\Program Files\DisplayXR\Plugins\<YourDevice>\` (a sibling of `Runtime\`, never inside it), write your row (`SetRegView 64`; `WriteRegStr` creates missing keys), finish. If you need a minimum runtime, check it only when `HKLM\Software\DisplayXR\Runtime` has an `InstallPath`.
- **Release your DLL before replacing it.** The service maps it: use Windows Restart Manager on your files; if you must start the service yourself, start it through `explorer.exe` so it does not run elevated.
- **Honor `/S`.** No modal dialogs in silent mode; bundles install you silently.
- **Your device's own driver or pairing software** is your installer's business, and the provider must load without it (decline cleanly, or report `ABSENT`).

## 15. Testing

Everything below runs on a machine with the runtime installed or built from source, no 3D display needed.

1. **Is it registered?** `displayxr-cli input list [--json]` lists every registered provider without loading any DLL: ProbeOrder, `Enabled` and its source, the preferred provider, the one ranked first.
2. **Does it load, negotiate, probe and claim the roles?** `displayxr-cli selftest` runs the real loader and arbitration headlessly. With your provider registered and reporting `PRESENT`, it asserts a hand-controller role device with a valid interaction profile on **each hand your provider supplied** — for a stylus that is one hand; the other stays with the fallback and is reported `n/a`. A provider that fails to load (missing entry point, ABI mismatch, refused provenance) fails the check. Absence never fails it.
3. **Make yours win.** `displayxr-cli input use <id>` (per user) prefers your provider ahead of ProbeOrder; `input reset` undoes it; `input disable <id>` takes you out. A running service re-ranks live.
4. **Settings.** `displayxr-cli input options <id>` shows every key you declared and where its value comes from.
5. **Haptics and hot-plug.** `displayxr-cli input haptic-test [seconds]` pulses the hand-role devices and prints every role `generation_id` change — unplug and replug your dongle while it runs to watch the role move to the fallback and back.
6. **Compare with the reference stylus.** Enable the in-tree `sim_stylus` provider (the runtime installer registers it; on a from-source Windows build run `scriptsegister_dev_plugin.bat input stylus` elevated) — opt-in, like `sim_input`: `displayxr-cli input set sim-stylus ENABLE 1` (or `DXR_INPUT_SIM_STYLUS_ENABLE=1` in the hosting process); `displayxr-cli input options sim-stylus` lists its other settings — and run the same checks. It exercises the profile, the dominant-hand type and the glass-registered frame with no hardware. Register yours at a lower ProbeOrder and confirm you win while present and fall back to it when absent.
7. **Prove an app sees *your* device.** `displayxr-cli input emulated off` withholds the keyboard/mouse emulated pair, so with your provider absent the hand roles read *unassigned* rather than the emulated controllers. `input emulated on` restores it. `HKLM\Software\DisplayXR\Input\ForceQwerty = 1` skips all providers — the fastest way to prove a symptom is yours.
8. **The handedness flip.** Set `HKCU\Software\DisplayXR\InputProviders\Policy\Handedness` to 1 and confirm selftest's left-hand row names your stylus (and the log line `is a dominant-hand tool -> LEFT hand`); delete the value afterwards. `input list` does not load providers, so it cannot show the hand.
9. **Both paths.** Run your app checks in-process and again with `XRT_FORCE_MODE=ipc` against a running `displayxr-service.exe`; roles, the rig and presence live service-side there. Remember the service reads **its own** environment, not the app's.
10. **On the glass.** With a real display, touch the nib to known points of the panel (centre, corners) in a windowed app and confirm the app's cursor sits under it; then fly the rig (WASD) and confirm the pen stays on the glass.

## 16. How far your device reaches: native apps and engines

- **Native OpenXR apps** reach your device through standard actions the moment the runtime has it. For the stylus profile the app enables `XR_LOGITECH_mx_ink_stylus_interaction` and suggests bindings for `/interaction_profiles/logitech/mx_ink_stylus_logitech`; an app that only knows `khr/simple_controller` gets your fallback profile (§5).
- **Unity and Unreal apps** reach the runtime through the DisplayXR engine plug-ins ([`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity), [`displayxr-unreal`](https://github.com/DisplayXR/displayxr-unreal)), which own the OpenXR session for the engine. Those plug-ins must suggest bindings, sync actions and forward haptics for a provider device to appear in the engine's input system; check the plug-in version's release notes for stylus and controller support before promising it to engine developers. Nothing about this changes your provider.
- **Legacy apps** (OpenXR apps written for headsets, OpenVR titles through OpenComposite) see whatever profile they already suggest — usually a VR controller profile. A stylus that also declares a matching `binding_profiles` entry works there as a controller.

## 17. Checklist

- [ ] One exported symbol, `xrtInputPluginNegotiate`; returns `XRT_INPUT_PLUGIN_API_VERSION_CURRENT`; `struct_size = sizeof(struct xrt_input_plugin_iface)`.
- [ ] `iface->id` equals your registry subkey / manifest id.
- [ ] `probe` is cheap and succeeds whenever the provider is installed and enabled.
- [ ] `get_presence` implemented, non-blocking, transport-only.
- [ ] Transport and SDK state are process-scoped; devices are cheap.
- [ ] Stylus: `XRT_DEVICE_MX_INK_STYLUS`, `XRT_DEVICE_TYPE_DOMINANT_HAND_CONTROLLER`, `XRT_TRACKING_TYPE_RIG_LOCAL_PHYSICAL`; tip pose in the extension's frame; force 0–1.
- [ ] Poses are honest volume poses: no rig, no navigation, no parallax composed in.
- [ ] `get_tracked_pose` predicts for the requested time from one latched publication.
- [ ] `set_output` declared and non-blocking (a no-op if you have no actuator).
- [ ] Every knob read through `host->get_setting` and declared in `describe_settings`; no `getenv`.
- [ ] Every host slot NULL-checked and `struct_size`-checked.
- [ ] Built against the runtime release you target (the device struct is not ABI-versioned, §3).
- [ ] Signed, or the installer writes and removes the allowlist row for the exact bytes it installs.
- [ ] Installer owns only its own rows and files, works without the runtime, honors `/S`.
- [ ] `input list`, `selftest`, `input haptic-test` and an IPC run all pass; hot-plug moves the role and back.

## Related

- [Input-provider discovery spec](../specs/runtime/input-provider-discovery.md) — the normative contract.
- [`xrt_input_plugin_iface` reference](../reference/xrt_input_plugin_iface.md) — the vtable, the host iface and device obligations.
- [ADR-034](../adr/ADR-034-input-provider-plugins.md) — input providers, and Amendments 1–8 (presence, the rig rule, per-hand arbitration, navigation, per-app selection, canvas-relative hands, the stylus).
- [ADR-054](../adr/ADR-054-component-model.md) — the component model: verbs, settings, provenance.
- [ADR-063](../adr/ADR-063-input-provider-device-abi-hardening.md) — hardening the provider-device ABI (Proposed).
- [Vendor plug-in onboarding](vendor-plugin-onboarding.md) — the display-processor side.
