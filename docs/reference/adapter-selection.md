# GPU adapter selection (`DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU`)

**Status: SUPPORTED client-facing contract** (not a debug knob). Both variables
shipped in **v2.2.4** (#821); the shared `scanout` keyword landed with #918
Phase 0 (closing #846). They follow the same compatibility promise as the
`XR_DXR_*` extensions: names, accepted values, and selection semantics below
will not be renamed, dropped, or changed in meaning without a major-version
bump and a deprecation note here. Client software (e.g. the Unity plugin's
user-facing *Target GPU* setting, displayxr-unity#242) may build on them.

## Why they exist

On a hybrid (iGPU + dGPU) machine the runtime's default adapter suggestion is
the **render adapter** resolved by the ADR-037 §2 capability ranking —
dedicated VRAM, then adapter type, excluding software/remote adapters and
anything below the required feature level — which on such a machine is the
discrete GPU. (Before #918 this was `EnumAdapterByGpuPreference(0,
HIGH_PERFORMANCE)` on D3D and `device_type_priority()` on Vulkan; both are now
downstream of the one resolver, `src/xrt/auxiliary/d3d/d3d_render_adapter.*`.)
A client that
renders on the *other* adapter then diverges from the session device and the
cross-adapter eye bridge presents black (displayxr-unity#240). These variables
steer the runtime's choice so client and session land on the same silicon.

## `DXR_D3D_FORCE_GPU` (Windows, D3D11/D3D12)

Overrides the adapter LUID the runtime suggests via
`xrGetD3D11GraphicsRequirementsKHR` / `xrGetD3D12GraphicsRequirementsKHR`.
Read in `env_forced_adapter()`
(`src/xrt/auxiliary/d3d/d3d_render_adapter.cpp`), i.e. *inside* the render
adapter resolver — so the override travels through the same code path as the
default policy rather than around it, and the resulting choice reports whether
the override or the ranking decided.

| Value | Meaning |
|---|---|
| `scanout` | The adapter that owns the output the 3D panel is scanned out on — resolved per machine, see below |
| `igpu` / `integrated` | Hardware adapter with the **least** dedicated VRAM |
| `dgpu` / `discrete` | Hardware adapter with the **most** dedicated VRAM |
| `0`, `1`, … | DXGI adapter by enumeration index |
| unset / invalid / no match | Normal selection; invalid values WARN and are ignored (a copied-around env var can never brick an app) |

**VRAM classification is deliberate.** The keywords are *not* implemented with
`EnumAdapterByGpuPreference`: a per-app `UserGpuPreferences` registry entry
overrides the preference *argument* (with `GpuPreference=2` set,
`MINIMUM_POWER` still returns the discrete GPU first — observed on hardware).
Dedicated-VRAM ordering is immune to registry state, so the keywords keep
their meaning regardless of per-app GPU preferences. WARP/software adapters
are never selected.

### Timing — settable in-process before XR init

The variable is read **lazily at `xrGetD3DxxGraphicsRequirementsKHR` time**,
not at DLL load or `xrCreateInstance`. An in-process client (engine plugin,
provider) can therefore set it after its own startup and before XR
initialization, and it will be honored.

### The `getenv()` caveat (in-process clients)

The runtime reads the variable with CRT `getenv()`, which consults the
**CRT's cached environment table**, captured per CRT instance.
`SetEnvironmentVariableW` — which is what .NET's
`Environment.SetEnvironmentVariable` calls — updates only the Win32
environment block and does **not** refresh any CRT cache, so a client setting
the variable that way is **silently ignored**. In-process clients must set it
through the CRT: call `_putenv_s("DXR_D3D_FORCE_GPU", "igpu")` from native
code (managed clients need a native export that does this — see
displayxr-unity#243). Launcher scripts and parent processes are unaffected:
a child process's CRT captures the inherited environment at startup, so
`set` / `Environment` blocks in `.bat` files work as expected.

## `DXR_VK_FORCE_GPU` (all platforms, Vulkan)

Overrides the physical device the runtime selects (and thus what
`xrGetVulkanGraphicsDeviceKHR` reports). Read in `env_forced_gpu_index()`
(`src/xrt/auxiliary/vk/vk_bundle_init.c`).

| Value | Meaning |
|---|---|
| `scanout` | The device whose `VkPhysicalDeviceIDProperties::deviceLUID` is the panel's scanout adapter (**Windows only**) — see below |
| `igpu` / `integrated` | First device with `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` |
| `dgpu` / `discrete` | First device with `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU` |
| `0`, `1`, … | `vkEnumeratePhysicalDevices` index |
| unset / invalid / no match | Normal selection; WARN and ignore |

Vulkan classifies by driver-reported device type (reliable in VK, unlike
DXGI preference ordering). The same `getenv()` caveat applies to in-process
clients on Windows.

## The `scanout` keyword

Shipped in **#918 Phase 0**, closing the gap tracked as #846. Accepted by
**both** variables, with identical meaning: *the adapter that owns the output
the 3D panel is scanned out on*.

**It is not an alias for `igpu`.** The rule is **weave next to scanout**, and
which silicon that is is a property of the machine, not of the keyword:

| Machine | `scanout` resolves to |
|---|---|
| Panel wired to the integrated display controller (typical SR desktop/laptop) | the **iGPU** |
| MUX'd laptop in discrete mode, or a panel wired to the discrete card | the **dGPU** |
| Single-GPU box | that GPU (a no-op) |

That is the whole point of having a keyword rather than telling users to type
`igpu`: the same launcher script, the same Unity *Target GPU* setting, and the
same `.bat` file do the right thing on every machine.

### How it resolves

One resolver, shared by both variables
(`src/xrt/auxiliary/d3d/d3d_scanout_helpers.{h,hpp,cpp}`):

1. Take the panel rect the display-processor plug-in reports
   (`display_screen_left/top` + `display_pixel_width/height`).
2. Compute its **centre point** and call
   `MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST)`.
3. Walk the DXGI hardware adapters (WARP/software skipped) and their outputs;
   the adapter whose `DXGI_OUTPUT_DESC::Monitor` equals that `HMONITOR` is the
   scanout adapter. HMONITOR equality keeps the whole comparison inside one
   process's GDI coordinate space.

**The centre point is deliberate, and edge-exact rect matching is deliberately
avoided.** `display_pixel_width/height` are *physical panel pixels*, while
`display_screen_left/top` are virtual-screen coordinates that Windows
DPI-virtualizes for a non-DPI-aware process. The two are therefore not
guaranteed to be in the same units, and an edge comparison would miss on a
scaled display. A centre point plus `DEFAULTTONEAREST` tolerates that error.

### Where each variable consumes it

- **D3D** — `oxr_d3d_get_requirements()`
  (`src/xrt/state_trackers/oxr/oxr_d3d.cpp`) calls `getRenderAdapter()` and
  passes the panel rect straight through: by
  `xrGetD3D11/12GraphicsRequirementsKHR` time it is already on `xsysc->info`.
  The resolver forwards `scanout` to `getScanoutAdapter()`, so there is still
  exactly one `QueryDisplayConfig` implementation.
- **The D3D11 service's own ingest device** — `comp_d3d11_service.cpp`'s
  `create_system` calls the same `getRenderAdapter()` (#1153), so the service's
  ingest adapter — the one ADR-037 §7 says clients must share, and the LUID the
  service publishes as `client_d3d_deviceLUID` — is resolved by the ranking
  rather than pinned to `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`. **The override
  reaches it too**, which is what makes an all-on-scanout service arm buildable:
  `DXR_D3D_FORCE_GPU=scanout` in the *service's* environment moves ingest to the
  scanout adapter, and if the clients are not forced the same way the
  configuration is deliberately cross-adapter. That is logged loudly on both
  sides (`ADR-037 §7: the service INGEST device was FORCED …` in the service
  log, `CROSS-ADAPTER by explicit override; proceeding` in the client's) and
  **never blocked** — an explicit §4 override outranks the shared-adapter
  assumption.
- **Vulkan** — `env_forced_gpu_index()` lives in `aux_vk`
  (`src/xrt/auxiliary/vk/vk_bundle_init.c`), which can see neither the plug-in
  nor DXGI. So the layer that owns both — `target_instance.c`, which already
  queries the plug-in for the panel refresh rate before creating the
  compositor — resolves the LUID and passes it down the existing creation path
  (`null_compositor_create_system_with_dims` → `comp_vulkan_arguments` →
  `vk_bundle::scanout_adapter_luid`). `env_forced_gpu_index()` then only
  compares LUIDs, staying free of any Win32 dependency. The **default**
  (non-override) ranking arrives by the same route: `target_instance.c` also
  resolves `d3d_render_adapter_luid()` into `vk_bundle::render_adapter_luid`,
  which `select_physical_device()` matches against
  `VkPhysicalDeviceIDProperties::deviceLUID` — among the physical devices
  Vulkan enumerated, so it ranks rather than bypasses VK's own requirements. That single hook
  covers both the app suggestion (`xrGetVulkanGraphicsDeviceKHR`, which reports
  the runtime's `client_vk_deviceUUID`) and the compositor's own `vk_bundle`,
  so client and session land on the same silicon.

### Failure semantics

Same contract as every other value (#845): **any** resolution failure — no
plug-in display info, a zero-sized panel, DXGI unavailable, no adapter claiming
the panel's monitor, no Vulkan device reporting that LUID — is **one WARN and
a fall back to normal selection**. A copied-around env var can never brick an
app. The resolution happens only when the keyword is actually requested, so
the DXGI walk costs nothing otherwise.

**Windows-only semantics.** On macOS / Linux / Android there is no scanout
adapter to resolve; `DXR_VK_FORCE_GPU=scanout` WARNs once and is ignored.
(`DXR_D3D_FORCE_GPU` is Windows-only to begin with.)

## Interaction with the output-device split (`DXR_WEAVE_ON_SCANOUT`)

> **`DXR_WEAVE_ON_SCANOUT` is a KILL SWITCH, not an opt-in — since #918 Phase 3.**
> The output-device split now engages **automatically** wherever the scanout
> adapter differs from the render adapter and the path implements it (ADR-037
> §1: the split is not a mode to opt into, it is what the placement rule
> degenerates to when the two adapters differ). Set `DXR_WEAVE_ON_SCANOUT=0` —
> or `f…`, `n…`, `off` — to force the old single-adapter behaviour. `=1` still
> works and is now a no-op restatement of the default, so nothing that already
> sets it changed meaning.

These two variables answer *different* questions and can cancel each other out.
`DXR_D3D_FORCE_GPU` decides which adapter the **whole session** — app render,
weave and present — lives on. The split instead **separates** them: the app keeps
rendering wherever its device already is, while the swapchain, the display
processor, the HUD and the repaint loop move to the scanout adapter, with the
composited atlas crossing once per app frame.

The split's activation gate is a LUID comparison, so **forcing the app onto the
scanout adapter makes the split a no-op**: `DXR_D3D_FORCE_GPU=scanout` (or
`=igpu` where those are the same adapter) logs `split=0 reason=same_adapter` and
takes the stock single-device path. That is correct, not a failure — everything
is already local to the panel, which is exactly what the split was going to
arrange. Use `DXR_D3D_FORCE_GPU=scanout` when the app can afford to render on
the iGPU; otherwise leave it alone and let the split keep the dGPU rendering.

### Where the split engages automatically

| path | auto-engages? | `reason=` when it does not |
|---|---|---|
| In-process **D3D11** (`_handle`, `_hosted`) | yes | `no_hwnd` (offscreen) · `shared_texture_session` (`_texture`) · `render_unresolvable` · `scanout_unresolvable` · `same_adapter` · `stage_a_failed` · `dp_refused_scanout` (the plug-in declined a weaver on the scanout adapter — asked inside Stage A, so the split never engages, ADR-037 §3a) · `killed_by_env` |
| **D3D11 service** (`_ipc`, any client API) | yes, per presenter | `presenter_ineligible,weave_on_ingest` (`CLIENT_TEXTURE` / self-presenting — the client owns the present, ADR-037 §7, and its display processor follows it onto the render device, #1172) · `legacy_standalone` · `no_panel_dimensions` · `same_adapter` · `stage_a_failed` · `killed_by_env` |
| In-process **D3D12** (Unity, Unreal), projection-only | yes | as D3D11, plus `layers_unsupported` (a zones / Local2D / mask frame retires the split for the session) and `dp_refused_scanout` (ADR-037 §3a) |
| In-process **Vulkan** | **no** | `api_unsupported` — rung 2, everything on the render adapter |
| In-process **OpenGL** | **no** | `api_unsupported` — and ADR-037 §5: OpenGL exposes no device-*selection* API at all (it does usually expose the adapter's *identity* — see below) |
| Metal / Android | n/a | single-adapter; the rule degenerates |

Vulkan and OpenGL take rung 2 **unconditionally**, and neither can reach a
half-engaged state: nothing in either compositor consults a scanout adapter,
allocates a bridge, or creates a second device. They log the placement line and
render exactly as before.

### OpenGL: identity yes, selection no (#1159)

The GL compositor creates D3D11 devices for two interop paths — the
transparency (DComp) present bridge and the device that opens the app's shared
texture. Both used to pass a **NULL adapter**, i.e. DXGI enumeration order, so
on a hybrid box the interop could silently become a cross-adapter copy. They now
resolve an adapter deliberately, best evidence first:

1. **`GL_EXT_memory_object_win32` → `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)`.**
   The driver reporting its own D3D LUID. Exact, not a guess; available on
   current NVIDIA / Intel / AMD Windows drivers. This is also what the GL
   `weave placement:` line prints as `render=`.
2. **`GL_VENDOR` → PCI vendor id**, used only when it names exactly one adapter
   (the usual iGPU+dGPU box is two different vendors, so it does). Heuristic.
3. **`GL_RENDERER` vs the DXGI descriptions**, to split a same-vendor tie. Used
   only when unambiguous. Heuristic.
4. **The ADR-037 §2 resolver.** Not "the GL adapter" — a ranked, overridable
   choice instead of enumeration order, and logged as a fallback.

Grep a GL session's log for `#1159`:

```
ADR-037 §5 GL placement is OS-ADVISORY: … GL context is on LUID=… (NVIDIA GeForce RTX 3080/PCIe/SSE2); ADR-037 §2 would pick 'NVIDIA …' LUID=… (most VRAM) — MATCH. …
ADR-037 §5 GL interop device (transparency present bridge): 'NVIDIA …' LUID=… (GL context LUID (GL_EXT_memory_object_win32)) (#1159)
ADR-037 §5 GL interop device (shared-texture upload): 'NVIDIA …' LUID=… (GL context LUID (GL_EXT_memory_object_win32)) (#1159)
```

and, when they genuinely differ, one WARN naming both:

```
ADR-037 §5: GL interop device (transparency present bridge) is on LUID=… but the GL context runs on LUID=… — CROSS-ADAPTER interop; every share/copy crosses the bus, every frame (#1159)
```

For the shared-texture device the adapter is not a preference but a
**requirement**: a D3D11 shared handle can only be opened by a device on the
adapter that created it, so `OpenSharedResource` succeeding is itself the proof.
If the resolved adapter cannot open it, the remaining hardware adapters are
tried in turn and the one that works is reported as a `CROSS-ADAPTER handoff` —
a mismatch degrades loudly instead of failing session creation.

None of this makes GL placement enforceable. The runtime still cannot move a GL
context; the per-exe `UserGpuPreferences` pin and `NvOptimusEnablement` remain
the only levers, and they are the OS's.

## Checking where the weave actually runs

Neither variable has to be *guessed at*. Two places report the answer:

- **`displayxr-cli info` → the `GPU topology` section.** Headless (no app, no
  compositor): it lists the hardware adapters with their LUIDs and dedicated
  VRAM, names the adapter that scans out the panel, names the adapter the
  runtime would suggest for rendering by default, prints the verdict line
  `weave-on-scanout topology: APPLIES (render != scanout)` / `does not apply
  (single adapter / same adapter)`, and shows the current
  `DXR_WEAVE_ON_SCANOUT` value. `displayxr-cli selftest` carries the same
  verdict line as one informational (never-failing) check. Since Phase 3 the
  `service split:` line answers "did anyone turn it **off**", so on an ordinary
  box it reads `WOULD ENGAGE` and the interesting bug report is the one that
  says `KILLED`. On a hybrid box with the panel on the iGPU it reads:

  ```
   :: GPU topology (#918 — does the weave cross adapters to reach the panel?)
        adapters:     2
          [0] NVIDIA GeForce RTX 3080 Laptop GPU LUID=00000000:00024f0b  dedicated VRAM 8018 MB
          [1] Intel(R) UHD Graphics            LUID=00000000:00024bbf  dedicated VRAM 128 MB
        panel scanout: 'Intel(R) UHD Graphics' LUID=00000000:00024bbf
        render (default suggestion): 'NVIDIA GeForce RTX 3080 Laptop GPU' LUID=00000000:00024f0b
        weave-on-scanout topology: APPLIES (render != scanout)
        DXR_WEAVE_ON_SCANOUT=<unset> (kill switch; the split is ON by default)
        service split: WOULD ENGAGE (default on, render != scanout); ingress adaptive (default)
        service ingest: 'NVIDIA GeForce RTX 3080 Laptop GPU' LUID=00000000:00024f0b (most VRAM) — the adapter clients must share (ADR-037 §7)
  ```

  The `service ingest` line (#1153) is the one that answers *"where will the
  service put the device my shared textures have to land on?"* — it runs the
  same resolver the service runs, in the same environment, so an override arm
  is verifiable **before** the service is started:

  ```
        service ingest: 'Intel(R) UHD Graphics' LUID=00000000:00024bbf (env-forced: scanout) — the adapter clients must share (ADR-037 §7); DXR_D3D_FORCE_GPU=scanout HONOURED
  ```

  `HONOURED` / `set but NOT honoured` is read off the resolver's provenance,
  not off the environment, so a value the resolver rejected (a stray trailing
  space is enough) reports as rejected instead of as applied.

- **The session log**, for what a *specific* app actually got. **Every** session
  — D3D11, D3D12, the service, Vulkan and OpenGL — logs exactly one
  `weave placement:` WARN, formatted in one place (`aux_d3d`) so it is literally
  the same string on every path. Grep the newest
  `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log` for `weave placement`. It
  names the render adapter, the panel's scanout adapter, the regime, and — when
  the split is off — **why**:

  ```
  weave placement: render='NVIDIA …' LUID=…, panel scanout='Intel …' LUID=… — weave/present on the SCANOUT adapter (split=1) (#918)
  weave placement: render='…' LUID=…, panel scanout='…' LUID=… — render and scanout share one adapter; weave is local (split=0 reason=same_adapter) (#918)
  weave placement: render='…' LUID=…, panel scanout='…' LUID=… — weave on the RENDER adapter; every present crosses adapters to reach scanout (split=0 reason=api_unsupported) (#918)
  weave placement: render='…' LUID=…, panel scanout=UNRESOLVED — cannot tell whether the weave crosses adapters (split=0 reason=scanout_unresolvable) (#918)
  weave placement: render=UNKNOWN, panel scanout='…' LUID=… — weave on the RENDER adapter; … (split=0 reason=api_unsupported) (#918)
  ```

  `render=UNKNOWN` is OpenGL only, and is the literal truth rather than a
  failed lookup — see ADR-037 §5. Since #1159 it is also **rare**: GL prints the
  real render LUID whenever the driver reports `GL_DEVICE_LUID_EXT`, and
  `UNKNOWN` means the driver did not.

  **The LAST `weave placement:` line in a log is the truth.** The in-process
  D3D12 path can retire an engaged split mid-session (a zones/Local2D/mask
  frame, or a display processor that declines the scanout adapter) and emits
  `weave placement: CHANGED — … (split=0 reason=…)` when it does. Everything
  else emits the line once, at session create — including the in-process D3D11
  path on a `dp_refused_scanout`, which is discovered inside Stage A (the DP is
  asked before the split commits) and so is already reflected in the first and
  only line.

  The `#918 output-device split ...` lines are separate and appear only when
  Stage A actually ran; the `weave placement:` line is always there. The
  service additionally reports the live state on its periodic
  `[RENDER] split=0 reason=<token>` line, using the same tokens.

  The reason tokens are a closed set (`comp_split_gate.h`): `killed_by_env`,
  `same_adapter`, `scanout_unresolvable`, `render_unresolvable`, `no_hwnd`,
  `shared_texture_session`, `no_panel_dimensions`, `legacy_standalone`,
  `api_unsupported`, `presenter_ineligible`, `weave_on_ingest`,
  `stage_a_failed`, `dp_refused_scanout`, `layers_unsupported`.
  `stage_a_failed` always has the specific failure spelled out in the WARN
  immediately above it.

  `weave_on_ingest` is the companion of `presenter_ineligible` and names the
  **device** half rather than the placement half (#1172). An ineligible
  presenter must not merely be *placed* off the scanout adapter, it must never
  **touch** it: its atlas, its shared input texture and its handback all live on
  the render device, so the display processor that weaves them does too — a
  display processor of its own, on its own window, rather than the shared panel
  DP (whose device follows whichever presenter currently owns the panel, and is
  the scanout one whenever that is an eligible presenter). Handing a
  scanout-adapter weaver a render-adapter texture is not a D3D11 error; it
  faults inside the vendor SDK.

## Choosing a value

- **Overlay-class / weaving apps on hybrid machines**: use `scanout`. It is the
  portable spelling of "the adapter that scans out the 3D panel", which on most
  SR boxes is the iGPU but is the dGPU on a MUX'd laptop — prefer it over
  hardcoding `igpu`. Baked composition (`DXR_PRESENT_OPAQUE=1`) *requires* the
  scanout adapter — cross-adapter Windows Graphics Capture delivers black
  frames (see `docs/architecture/transparency-modes.md`).
- **Render-heavy apps**: the default (discrete) is usually right; the present
  bridge handles cross-adapter scanout on the live-composition path.
