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
the discrete GPU (`EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE)` on D3D;
`device_type_priority()` ranks `DISCRETE_GPU` first on Vulkan). A client that
renders on the *other* adapter then diverges from the session device and the
cross-adapter eye bridge presents black (displayxr-unity#240). These variables
steer the runtime's choice so client and session land on the same silicon.

## `DXR_D3D_FORCE_GPU` (Windows, D3D11/D3D12)

Overrides the adapter LUID the runtime suggests via
`xrGetD3D11GraphicsRequirementsKHR` / `xrGetD3D12GraphicsRequirementsKHR`.
Read in `env_forced_d3d_adapter()` (`src/xrt/state_trackers/oxr/oxr_d3d.cpp`).

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

- **D3D** — `env_forced_d3d_adapter()` in
  `src/xrt/state_trackers/oxr/oxr_d3d.cpp` calls the resolver directly: by
  `xrGetD3D11/12GraphicsRequirementsKHR` time the panel rect is already on
  `xsysc->info`.
- **Vulkan** — `env_forced_gpu_index()` lives in `aux_vk`
  (`src/xrt/auxiliary/vk/vk_bundle_init.c`), which can see neither the plug-in
  nor DXGI. So the layer that owns both — `target_instance.c`, which already
  queries the plug-in for the panel refresh rate before creating the
  compositor — resolves the LUID and passes it down the existing creation path
  (`null_compositor_create_system_with_dims` → `comp_vulkan_arguments` →
  `vk_bundle::scanout_adapter_luid`). `env_forced_gpu_index()` then only
  compares LUIDs, staying free of any Win32 dependency. That single hook
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

## Choosing a value

- **Overlay-class / weaving apps on hybrid machines**: use `scanout`. It is the
  portable spelling of "the adapter that scans out the 3D panel", which on most
  SR boxes is the iGPU but is the dGPU on a MUX'd laptop — prefer it over
  hardcoding `igpu`. Baked composition (`DXR_PRESENT_OPAQUE=1`) *requires* the
  scanout adapter — cross-adapter Windows Graphics Capture delivers black
  frames (see `docs/architecture/transparency-modes.md`).
- **Render-heavy apps**: the default (discrete) is usually right; the present
  bridge handles cross-adapter scanout on the live-composition path.
