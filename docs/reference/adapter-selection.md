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

These two variables answer *different* questions and can cancel each other out.
`DXR_D3D_FORCE_GPU` decides which adapter the **whole session** — app render,
weave and present — lives on. `DXR_WEAVE_ON_SCANOUT=1` (#918 Phase 1, D3D11)
instead **splits** them: the app keeps rendering wherever its device already is,
while the swapchain, the display processor, the HUD and the repaint loop move to
the scanout adapter, with the composited atlas crossing once per app frame.

The split's activation gate is a LUID comparison, so **forcing the app onto the
scanout adapter makes the split a no-op**: `DXR_WEAVE_ON_SCANOUT=1` together with
`DXR_D3D_FORCE_GPU=scanout` (or `=igpu` where those are the same adapter) logs
`scanout adapter ... IS the app's adapter — split is a no-op` and takes the stock
single-device path. That is correct, not a failure — everything is already local
to the panel, which is exactly what the split was going to arrange. Use one or
the other: `DXR_D3D_FORCE_GPU=scanout` when the app can afford to render on the
iGPU, `DXR_WEAVE_ON_SCANOUT=1` when it cannot and the dGPU has to keep the
rendering.

## Checking where the weave actually runs

Neither variable has to be *guessed at*. Two places report the answer:

- **`displayxr-cli info` → the `GPU topology` section.** Headless (no app, no
  compositor): it lists the hardware adapters with their LUIDs and dedicated
  VRAM, names the adapter that scans out the panel, names the adapter the
  runtime would suggest for rendering by default, prints the verdict line
  `weave-on-scanout topology: APPLIES (render != scanout)` / `does not apply
  (single adapter / same adapter)`, and shows the current
  `DXR_WEAVE_ON_SCANOUT` value. `displayxr-cli selftest` carries the same
  verdict line as one informational (never-failing) check. On a hybrid box
  with the panel on the iGPU it reads:

  ```
   :: GPU topology (#918 — does the weave cross adapters to reach the panel?)
        adapters:     2
          [0] NVIDIA GeForce RTX 3080 Laptop GPU LUID=00000000:00024f0b  dedicated VRAM 8018 MB
          [1] Intel(R) UHD Graphics            LUID=00000000:00024bbf  dedicated VRAM 128 MB
        panel scanout: 'Intel(R) UHD Graphics' LUID=00000000:00024bbf
        render (default suggestion): 'NVIDIA GeForce RTX 3080 Laptop GPU' LUID=00000000:00024f0b
        weave-on-scanout topology: APPLIES (render != scanout)
        DXR_WEAVE_ON_SCANOUT=<unset>
  ```

- **The session log**, for what a *specific* app actually got. Every D3D11
  session logs exactly one `weave placement:` WARN naming the render adapter,
  the panel's scanout adapter, and where the weave runs — in all three states
  (`weave/present on the SCANOUT adapter`, `weave on the RENDER adapter; every
  present crosses adapters to reach scanout`, `render and scanout share one
  adapter; weave is local`), plus an explicit `panel scanout=UNRESOLVED` when
  the panel's adapter cannot be determined. It is emitted whether or not
  `DXR_WEAVE_ON_SCANOUT` is set, so a hybrid box quietly paying the
  cross-adapter present no longer produces a log indistinguishable from a
  single-adapter box that pays nothing. Grep the newest
  `%LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log` for `weave placement`.

  The `D3D11 output-device split ...` lines are separate and appear only when
  the split was actually attempted; the `weave placement:` line is always there.

## Choosing a value

- **Overlay-class / weaving apps on hybrid machines**: use `scanout`. It is the
  portable spelling of "the adapter that scans out the 3D panel", which on most
  SR boxes is the iGPU but is the dGPU on a MUX'd laptop — prefer it over
  hardcoding `igpu`. Baked composition (`DXR_PRESENT_OPAQUE=1`) *requires* the
  scanout adapter — cross-adapter Windows Graphics Capture delivers black
  frames (see `docs/architecture/transparency-modes.md`).
- **Render-heavy apps**: the default (discrete) is usually right; the present
  bridge handles cross-adapter scanout on the live-composition path.
