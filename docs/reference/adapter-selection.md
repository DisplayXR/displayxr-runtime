# GPU adapter selection (`DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU`)

**Status: SUPPORTED client-facing contract** (not a debug knob). Both variables
shipped in **v2.2.4** (#821) and follow the same compatibility promise as the
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
| `igpu` / `integrated` | First device with `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` |
| `dgpu` / `discrete` | First device with `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU` |
| `0`, `1`, … | `vkEnumeratePhysicalDevices` index |
| unset / invalid / no match | Normal selection; WARN and ignore |

Vulkan classifies by driver-reported device type (reliable in VK, unlike
DXGI preference ordering). The same `getenv()` caveat applies to in-process
clients on Windows.

## Choosing a value

- **Overlay-class / weaving apps on hybrid machines**: prefer the adapter that
  scans out the 3D panel (usually the iGPU). Baked composition
  (`DXR_PRESENT_OPAQUE=1`) *requires* it — cross-adapter Windows Graphics
  Capture delivers black frames (see `docs/architecture/transparency-modes.md`).
- **Render-heavy apps**: the default (discrete) is usually right; the present
  bridge handles cross-adapter scanout on the live-composition path.
- A future `scanout` keyword resolving "the adapter that owns the 3D panel's
  output" portably is tracked in #846.
