# Android Vulkan Extension Survey

Pre-Lume-Pad reference. Lists every Vulkan device extension the runtime
+ test app request at session bring-up, and predicts what's present on
the emulator (swiftshader-via-gfxstream) vs Lume Pad's Adreno GPU.

The point: when you plug in Lume Pad and run `scripts/android-smoketest.sh`,
**you should know in advance which `xrCreate*` step is expected to advance
past the emulator wall and which steps will be the first to touch
hardware-specific behavior**. If anything else fails, that's a bug worth
investigating.

## Lume Pad 2 GPU baseline

| | |
|---|---|
| SoC | Snapdragon 845 (Adreno 630, Lume Pad 1) or 888 (Adreno 660, Lume Pad 2) |
| Vulkan driver | Qualcomm Adreno Vulkan ICD (system, signed) |
| Vulkan API version | 1.1 minimum, 1.3 typical for Adreno 660+ |
| Source of truth | `vulkan.gpuinfo.org` reports for the exact device model |

## Required device extensions (xrCreateVulkanDeviceKHR side)

The runtime appends these to whatever the test app already requests.
Source: `src/xrt/state_trackers/oxr/oxr_vulkan.c::required_vk_device_extensions[]`
on Android (`XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER` +
`XRT_GRAPHICS_SYNC_HANDLE_IS_FD`).

| # | Extension | Adreno 6xx/7xx | Emulator (swiftshader) | Notes |
|---|---|:---:|:---:|---|
| 1 | `VK_KHR_dedicated_allocation` | ✅ | ✅ | Core in 1.1 |
| 2 | `VK_KHR_external_fence` | ✅ | ✅ | Core in 1.1 |
| 3 | `VK_KHR_external_memory` | ✅ | ✅ | Core in 1.1 |
| 4 | `VK_KHR_external_semaphore` | ✅ | ✅ | Core in 1.1 |
| 5 | `VK_KHR_get_memory_requirements2` | ✅ | ✅ | Core in 1.1 |
| 6 | `VK_ANDROID_external_memory_android_hardware_buffer` | ✅ | ❌ | Android-only. **First likely emulator-wall** |
| 7 | `VK_KHR_sampler_ycbcr_conversion` | ✅ | ⚠️ | Required by #6; transitively missing on emulator |
| 8 | `VK_KHR_maintenance1` | ✅ | ✅ | Core in 1.1 |
| 9 | `VK_KHR_bind_memory2` | ✅ | ✅ | Core in 1.1 |
| 10 | `VK_EXT_queue_family_foreign` | ✅ | ❌ | Required by #6; tied to AHardwareBuffer |

**Emulator failure mode (today, captured 2026-05-27):** at session bring-up,
`null_compositor_create_system_with_dims` calls `vk_create_device` and gets
`VK_ERROR_EXTENSION_NOT_PRESENT`. The runtime gracefully degrades and lets
`xrCreateInstance` succeed (sentinel fires). The test app's own
`xrCreateVulkanDeviceKHR` then fails with the same error a few calls later
once it tries to enable an extension swiftshader doesn't have. Both fails
are the same root cause: **extensions #6/#7/#10 are AHardwareBuffer-related
and swiftshader doesn't implement them.**

**Lume Pad prediction:** all 10 required extensions present. Both
`null_compositor`'s VK device AND the test app's VK device should succeed.

## Optional device extensions (silently skipped if missing)

Source: `src/xrt/state_trackers/oxr/oxr_vulkan.c::optional_device_extensions[]`
on Android.

| Extension | Adreno 6xx/7xx | Emulator | Notes |
|---|:---:|:---:|---|
| `VK_KHR_external_semaphore_fd` | ✅ | ✅ | Used for cross-process sync |
| `VK_KHR_external_fence_fd` | ✅ | ✅ | Used for cross-process sync |
| `VK_KHR_image_format_list` | ✅ | ✅ | Swapchain image format optimisation |
| `VK_KHR_timeline_semaphore` | ✅ | ⚠️ | Adreno added in 2020 drivers; older devices may miss |

## Null compositor's separate VK device

The runtime's null compositor builds its own VkDevice (the "internal"
device, separate from the app-facing one). Same list as above plus
`VK_KHR_swapchain` (line 90 of `null_compositor.c`). On Android the null
compositor doesn't need a present surface, so swapchain is harmless if
present but **its absence won't break headless operation** (only the
internal device init logs a WARN and the runtime falls through to the
fallback path).

## CNSDK plug-in's VK requirements

The CNSDK plug-in does NOT create its own VkDevice — it uses the device
the runtime hands it via the DP `create_dp_vk` factory. So all CNSDK VK
calls go through the test-app-created device, which already has the full
required-extension set above.

CNSDK's `leia_interlacer_vulkan_initialize` will fail at runtime if the
device is missing any of the extensions CNSDK uses internally. The CNSDK
0.7.28 source isn't fully transparent on this; check the published
`vulkaninfo` for Adreno 6xx and confirm at first session if the
interlacer errors out.

## Step-by-step expectation at first Lume Pad bring-up

Walk through these in order — the column heading is what should appear
in logcat at each milestone.

| Step | Log line | Emulator | Lume Pad expected |
|---|---|:---:|:---:|
| Loader init | `xrInitializeLoaderKHR -> XR_SUCCESS` | ✅ | ✅ |
| Broker resolves runtime | `OpenXR-Loader: Got runtime: package: …` | ✅ | ✅ |
| Plug-in dlopen | `plugin loader: active plug-in: id=leia-cnsdk …` | ✅ | ✅ |
| **Null compositor VK device** | `monado.create_device: VK_ERROR_EXTENSION_NOT_PRESENT` | ❌ (expected) | ✅ (no error log) |
| Instance creation | `ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS` | ✅ | ✅ |
| `xrGetSystem` | `System: "DisplayXR: Leia CNSDK (Android)"` | ✅ | ✅ |
| `xrGetVulkanGraphicsRequirements2KHR` | `Vulkan API: min=1.0.0 max=…` | ✅ | ✅ |
| `xrCreateVulkanInstanceKHR` | `xrCreateVulkanInstanceKHR -> XR_SUCCESS` | ✅ | ✅ |
| `xrGetVulkanGraphicsDevice2KHR` | `xrGetVulkanGraphicsDevice2KHR -> XR_SUCCESS` | ✅ | ✅ |
| **`xrCreateVulkanDeviceKHR`** | `xrCreateVulkanDeviceKHR -> XR_SUCCESS` (no `vk_result=-7`) | ❌ vk_result=-7 (expected) | ✅ first-touch hardware step |
| `xrCreateSession` | `xrBeginSession returned XR_SUCCESS` | (unreached) | ✅ |
| `leia_interlacer_vulkan_initialize` | DP creation log | (unreached) | ✅ if all CNSDK-needed exts present |
| First frame | `Atlas blit: view_count=2, hardware_3d=1` | (unreached) | ✅ |

## What to grep for if Lume Pad fails

```bash
# Which extension exactly is missing (Adreno driver will spell it out):
adb logcat | grep -i 'extension not found\|extension not supported\|VK_ERROR_EXTENSION_NOT_PRESENT'

# What Adreno actually reports:
adb logcat | grep -i 'vulkan' | grep -i 'apiVersion\|driverName\|deviceName'

# All Vulkan-related WARN+ERROR from the runtime:
adb logcat | grep -E 'monado\.(create_device|vk_|comp_)' | grep -E 'ERROR|WARN'
```

## How to update this doc

If Lume Pad reveals an extension we got wrong here:

1. Capture the exact error: `adb logcat | grep -A2 VK_ERROR_EXTENSION_NOT_PRESENT`
2. Look up the device's actual extension list at `vulkan.gpuinfo.org`.
3. Update the matching row's "Adreno" column with ❌ + a note on which Adreno gen lost support.
4. If the runtime needs to make the extension optional, change it in `src/xrt/state_trackers/oxr/oxr_vulkan.c` (move from `required_vk_device_extensions[]` to `optional_device_extensions[]`).
