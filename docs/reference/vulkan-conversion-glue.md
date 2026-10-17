# Vulkan conversion glue (macOS, desktop Linux)

The conversion core (`src/xrt/conversion/`, ADR-053) does no graphics work itself. It takes
image allocation, the S5 reference view synthesis and the S6 GPU depth readback through three
small vtables (`conv_image_alloc_ops`, `conv_viewsyn_ops`, `conv_depth_readback_ops`). On Windows
the D3D11 service fills them (`d3d11_service/d3d11_conv_gpu.cpp`, `d3d11_conv_viewsyn.cpp`). On
macOS and desktop Linux the Vulkan glue in `src/xrt/compositor/vk_conv/` fills them. This note
says what attaches where, which transports are real, and how the CPU lane fits in.

## What attaches where

| Piece | File | What it is |
|---|---|---|
| S5 stage | `vk_conv/vk_conv_viewsyn.{c,h}` + `vk_conv/shaders/conv_viewsyn_*.comp` | Port of the D3D11 stage, pass for pass, as GLSL compute shaders. It only records into the caller's command buffer: it owns no queue, takes no lock, and logs nothing per frame. |
| GPU glue | `vk_conv/vk_conv_gpu.{c,h}` | Allocation, CPU ↔ image copies, the S5 adapter and S6 readback, on a borrowed `vk_bundle` |
| Service attach | `ipc/server/ipc_server_conv_vk.c` | Creates the glue on the system compositor's Vulkan device once `init_all` has created the compositor, and installs it with `ipc_server_conv_install_gpu`. Detaches in `teardown_all` before the compositor is destroyed. |

On macOS and desktop Linux the service's system compositor is the multi system compositor over
the null compositor. The null compositor's `vk_bundle`, reached through the target service
(`comp_multi_system_get_vk()`), is the one service-wide Vulkan device. The shared spatial
surface, the macOS Vulkan weave backend and the desktop-Linux weave engine already run on it,
and the glue submits on its main queue under the same queue mutex. So macOS and Linux share one
glue, with MoltenVK underneath on macOS. That holds whichever macOS weave backend
(`DXR_WEAVE_MAC_BACKEND`) is picked, because the Metal backend owns its own device and the glue
never touches it.

The CMake gate is `XRT_HAVE_VULKAN AND (APPLE OR (XRT_HAVE_LINUX AND NOT ANDROID))`. It builds
`comp_vk_conv_viewsyn` and `comp_vk_conv`, and the `ipc_server` block defines
`XRT_HAVE_VK_CONV_SERVICE`.

**One I/O table on POSIX.** The client transport is the CPU I/O glue
(`ipc/server/ipc_server_conv_cpu.c`): spec-4 CPU bytes in and out, with a POSIX shm fd on Linux
and an IOSurface used as a byte buffer on macOS. It is flagged `cpu_only`, so there is no v3
route. The service installs it at start. The Vulkan glue then attaches under it, in this order:

1. `ipc_server_conv_cpu_io_set_gpu()` lends the CPU table the glue's CPU ↔ image copies
   (`vk_conv_gpu_write` / `vk_conv_gpu_read`). With them `publish_result` can read a GPU result
   (the composed views, a mirror) back into the export section, and `import_input_ex` can fill a
   GPU slot image. Layered (ARRAY) GPU results are refused, because the section holds one layer.
2. `ipc_server_conv_install_gpu()` installs image allocation, S5, S6 and the display's nominal
   viewing distance. Its `io` is the CPU table itself, so the install keeps the `cpu_only` route
   rather than replacing it, and logs "GPU glue installed under the CPU route (cpu_only)".

Detach reverses this: `install_gpu(NULL)` (the route, the core-routed streams, the allocator, the
stages), then `set_gpu(NULL)` (which waits for a copy in flight), then the glue is destroyed, and
then the CPU table is re-installed. `tests_conv_vk_gpu` runs the composition end to end: CPU
bytes in, a composed conversion, the GPU views exported as CPU bytes.

With this, a depth-only provider serves the composed tasks (`VIEWS_MONO`, `VIEWS_FROM_DEPTH`) on
POSIX.

Not installed yet:

- A GPU client transport (textures and fences to the app). The client gets CPU bytes only.
- The weave-rect bridge and the per-DP eyes sink (step C). Until they land, core streams read
  the nominal viewer.
- The `dp-lift` backend, because the display plug-in's lift slots are D3D11-only.

## Memory kinds: what is real

| Kind | Where | How |
|---|---|---|
| `XRT_CONV_MEM_CPU` | everywhere (the v1 lane) | the service's CPU allocator, used whenever the glue declines |
| `XRT_CONV_MEM_VK_OPAQUE_FD` | Linux, `VK_KHR_external_memory_fd` | dedicated `VkExportMemoryAllocateInfo`, `vkGetMemoryFdKHR` |
| `XRT_CONV_MEM_DMABUF` | Linux, `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` | `vk_create_exportable_dmabuf_image`, single-plane modifiers only, 1 layer, `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire/release around every glue access |
| `XRT_CONV_MEM_IOSURFACE` | macOS, `VK_EXT_metal_objects` + `VK_EXT_external_memory_metal` | `VkExportMetalObjectCreateInfoEXT(IOSURFACE)`, 1 layer |

**MoltenVK exposes neither `VK_KHR_external_memory_fd` nor any external-semaphore fd extension.**
So `VK_OPAQUE_FD` and the fd sync kinds are Linux-only, and IOSurface is the one macOS GPU
transport. `tests_conv_vk_gpu` verifies both directions on MoltenVK:

- **Export:** an IOSurface-backed `VkImage` written by the glue and read through
  `IOSurfaceLock`.
- **Import:** an IOSurface created on the CPU, imported through `VkImportMetalIOSurfaceInfoEXT`
  plus an `MTLTEXTURE` memory import, and read back.

The IOSurfaces MoltenVK creates report pixel format 0, so a consumer must take the format from
the descriptor, never from the surface.

All of these need only core Vulkan 1.0 plus widely available extensions, each checked at runtime
with the CPU lane as the fallback. Nothing assumes a particular GPU vendor. The first Linux
target is an Intel integrated GPU on Mesa (ANV); proprietary drivers come second.

## Who gets GPU memory

- **`RENDER_TARGET` (the composed views) and `MIRROR` images.** The runtime is their only
  consumer, so they are always GPU images. They are exportable when the platform can export the
  format and layer count (an IOSurface has one layer, so a 4-layer ARRAY target is
  device-private on macOS); otherwise they are device-private, with `handle =
  VK_CONV_GPU_NO_HANDLE`. `conv_stream` never registers either usage with a provider.
- **`INPUT` and `PROVIDER_OUT` images.** A provider imports these. They are GPU images only for
  kinds in `vk_conv_gpu_set_provider_kinds()`'s mask, which defaults to 0. No POSIX
  conversion-host GPU lane exists (`conv_host_proxy` refuses a GPU image for a satellite
  without one), so every provider stays on the CPU lane. A composed binding reports no import
  kinds at all, so its input and depth are CPU memory regardless.

The S5 adapter therefore accepts CPU colour and depth: it uploads them into per-stream scratch
images first. It also accepts a CPU target, which it reads back after the synthesis. A
CPU-memory provider result reaches GPU consumers through the core's #2030 mirror upload, as on
Windows.

## Sync

- **Images the glue hands out** always carry `XRT_CONV_SYNC_NONE`. Every glue call submits on
  the main queue and waits for its own work before it returns ("SYNC_NONE meaning the producer
  finished before hand-over", discovery spec §2.4).
- **Images the glue reads** may carry `XRT_CONV_SYNC_VK_SEMAPHORE_FD` (a timeline semaphore,
  opaque fd, imported once per `import_key`) or `XRT_CONV_SYNC_SYNC_FD` (a binary sync_file,
  imported temporarily). The glue waits on either on the GPU, in the same submit. These kinds
  are offered only when the device can import them, which means Linux.
- **S6 never stalls.** It keeps one copy in flight per stream, issues a new one at most every
  `U_LIFT_AUTOCONV_SAMPLE_PERIOD_NS`, and polls with `vkGetFenceStatus`, the same cadence as
  the D3D11 glue.

## View synthesis parity

`tests_conv_viewsyn_vk` re-implements every pass on the CPU, with the same intermediate
precision: RGBA16F rasters and an RGBA8 guide. It compares the GPU output against that
reference statistically, using the mean error and the share of pixels off by more than 4/255.
It also carries the D3D11 test's oracles: coverage, plateau parallax compared with
`u_conv_viewsyn_march`, the occluding edge, monotonic shift across views, and identity at the
source camera.

On MoltenVK the mean error is at most 0.013/255, and at most 2 silhouette pixels per view flip
by one march step. Both tests are clean under `VK_LAYER_KHRONOS_validation`.

The D3D11 stage cannot run on the same box. Both stages are pinned to the same `u_conv_viewsyn`
math and the same oracles; the CPU golden covers the passes those oracles cannot see (JBU,
snap, dilation, hole fill).

Two deliberate deviations from the D3D11 stage:

1. The upsampled raster is RGBA16F with only `.r` used. D3D11 uses R16F; R16F storage needs
   `shaderStorageImageExtendedFormats`, and the precision is the same half.
2. Each view is rendered into an RGBA16F scratch and blitted into the target. Any `BLIT_DST`
   format therefore works, and an SRGB target is encoded on the blit, as a D3D11 SRGB RTV is.

Colour is always sampled through a UNORM view. That is the stage contract, for browser parity.
