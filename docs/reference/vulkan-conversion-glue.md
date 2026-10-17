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
| Weave-rect helper | `vk_conv/vk_conv_weave.{c,h}` | Lifted weave rects (XR_DXR_lift §6) recorded into a weave engine's command buffer: snapshot, letterbox measurement, the pinned result drawn into the rect. See [Lifted weave rects](#lifted-weave-rects-step-c). |
| Service attach | `ipc/server/ipc_server_conv_vk.c` | Creates the glue on the system compositor's Vulkan device once `init_all` has created the compositor, and installs it with `ipc_server_conv_install_gpu`. Detaches in `teardown_all` before the compositor is destroyed. |

On macOS and desktop Linux the service's system compositor is the multi system compositor over
the null compositor. The null compositor's `vk_bundle`, reached through the target service
(`comp_multi_system_get_vk()`), is the one service-wide Vulkan device. The shared spatial
surface, the macOS Vulkan weave backend and the desktop-Linux weave engine already run on it,
and the glue submits on its main queue under the same queue mutex. So macOS and Linux share one
glue, with MoltenVK underneath on macOS. That holds whichever macOS weave backend
(`DXR_WEAVE_MAC_BACKEND`) is picked, because the Metal backend owns its own device and the glue
never touches it. The two meet only through the IOSurfaces the glue exports its results as (see
[the Metal backend](#the-macos-metal-weave-backend)).

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
  without one), so every provider stays on the CPU lane. A composed binding reports its depth
  stage's import kinds, so the same rule covers its input and depth; the composed views target
  carries no import kinds, because only the runtime touches it.

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

## Lifted weave rects (step C)

A weave client (the DisplayXR Browser) flags a weave rect "lift me" with
`XrWeaveSubmitLiftRectsDXR`. The wire is the one Windows uses. On macOS (both weave backends)
and desktop Linux the service lifts it with `vk_conv_weave`, following the D3D11 service's
policy. The macOS Metal backend records in Metal as a host client of the same helper (below).

### Wiring

| Piece | File | Role |
|---|---|---|
| Bridge | `ipc_server_conv_vk.c` | Resolves a weave client's wire stream id, a core-routed v3 id carrying `IPC_SERVER_CONV_STREAM_TAG`, to its core stream for that connection only (`ipc_server_conv_resolve_stream`). Passes spec 4's "match the weave" view count on (`ipc_server_conv_v4_match_weave`). |
| Spec-3 streams | `ipc_server_conv.c` | Under the CPU route (`cpu_only`) spec 3 stays off, except for SBS and NVIEW stream create and properties once the helper is attached (`ipc_server_conv_set_weave_rects`). Such a stream is fed only by lifted rects and read only by the weave. Its properties report SBS and NVIEW only. |
| Ops seam | `multi/comp_multi_weave_lift.{c,h}` | comp_multi is linked into the runtime client too, so it never links the conversion core. The IPC layer installs the helper's ops table (`comp_multi_weave_set_lift_ops`). The handler forwards `lift_weave_rects` to `comp_multi_weave_lift_set_rects`, which refuses a stream that is not an SBS or NVIEW stream of the connection. |
| Per-DP eyes sink | `comp_multi_set_conv_eyes_sink` → `ipc_server_conv_eyes_publish` / `_retire` | Every eye query comp_multi answers publishes the answering DP's eyes, keyed by the DP's address. That covers `multi_compositor_get_predicted_eye_positions` and each weave submit's eyes. A destroyed session DP or weave DP retires its key. A lifted rect's frame names the weave engine's DP, so its stream reads that DP's eyes, not the nominal viewer. |
| Engines | `comp_multi_weave_linux.c`, `comp_multi_weave_macos_vk.c` | Take the set at the top of each submit (one set, one submit), `begin` before recording, `record` per lifted rect, `complete` once the weave's fence has signalled. |
| Metal engine | `comp_multi_weave_macos_metal.m` + `comp_multi_weave_metal_lift.{h,m}` | `begin` with no Vulkan device (a host client), `host_plan` per lifted rect, records the plan in Metal, `host_done` per stream once the command buffer completed, then `complete`. |

Detach order: no new spec-3 streams, ops cleared (this waits for calls in flight), eyes sink
cleared, then the helper is destroyed. Destroying it idles the main queue and drops pins and
pending snapshots while the core's streams still exist. The glue teardown follows.

### Per lifted rect

1. **Snapshot.**
   - The source is the rect's 2D frame: the input in the batch layout, tile 0 of the crop in v6.
     A v6 frame with lifted rects never takes the zero-copy path.
   - The letterbox-cropped active area is blitted into a staging chain and capped to the
     `DXR_LIFT_MAX_INPUT_EDGE` long edge (default 1920, `u_lift_cap_dims`). Halving `LINEAR`
     blits do the downsample; a 2x step is an exact 2x2 box and samples nothing outside the
     rect. The final image is RGBA8 UNORM, copied to a host buffer in the engine's command
     buffer.
   - Once the frame completes, the pixels go into the stream's input slot. Begin, copy and
     commit happen in one call, so no slot is held across frames. The frame carries the D3D11
     path's sticky options, explicit viewpoints, view control, rig, the rect's display-space
     frame, the eyes DP and the active fractions.
   - POSIX providers run on the CPU lane, so this is GPU → host → slot, not GPU → GPU. A GPU
     slot is filled with `vk_conv_gpu_write` from the same buffer.
2. **Letterbox.** The 96 sampled columns and rows of the whole rect, bars included, are copied to
   a second buffer. On completion `u_lift_letterbox_profile_from_lines` reduces them on the CPU.
   It produces bucket for bucket the profile of the D3D11 reduction shader (pinned by
   `tests_lift_mailbox`), and feeds `u_lift_letterbox_update`.
3. **Result.**
   - The stream's latest GPU result (an S5 render target or a #2030 mirror) is pinned until the
     frame completes.
   - Each needed view is copied raw into a UNORM staging image, so an `_SRGB` result stays
     encoded. It is then stretched into the rect's active area of each tile.
   - Batch layout: the middle stereo pair, and the bars are the 2D input, flat in both tiles.
     v6: view *v* → tile *v*, proportionally when the counts differ.
   - Until the first result the batch layout writes the 2D frame into both tiles (flat). v6
     leaves the caller's tiles as drawn.

A submit without lifted rects records exactly what it recorded before.

**Differences from the Windows path.**

- **Draw order.** Lifted rects are drawn after the engine's squeezed rects, not in rect order.
  This only shows where a lifted rect overlaps another rect.
- **Rects without window metrics.** With no bound window geometry, the rect frame is the panel
  (`valid = false`). The bridge's `fit_rect` (`ipc_server_conv_submit_rect`, the windowless
  spec-4 rule) turns it into the rect's own aspect fitted in the panel. Without that, a composed
  conversion (a depth-only provider plus S5) fails every frame.

### The macOS Metal weave backend

The Metal backend (`DXR_WEAVE_MAC_BACKEND=metal`, or `auto` with a plug-in that exports only a
Metal display processor) owns its own `MTLDevice` and records no Vulkan commands. It is a
**host client** of `vk_conv_weave`: a client created without a device. The stream policy stays in
the helper, so the two macOS backends cannot drift apart.

1. **Plan.** Per lifted rect, `vk_conv_weave_host_plan` resolves the stream, decides the
   snapshot (letterbox crop, then the cap), fills the frame, and pins the latest result. These
   are the same functions `vk_conv_weave_record` uses. It answers with a plan: the source area
   and the snapshot extent, whether to copy the letterbox lines, and the result (its IOSurface,
   format, extent, view count, active area and the view each tile shows).
2. **Record** (`comp_multi_weave_metal_lift.m`, in the weave's own command buffer, after the
   squeezed rects on the batch layout and after the crop copy on v6):
   - The snapshot is resampled into an RGBA8 chain: halving linear passes (an exact 2x2 box),
     then the final pass. The final pass is texel-exact when 1:1, and no pass samples outside the
     area. It is then copied into a shared `MTLBuffer`. The letterbox lines go into a second one.
   - The result is wrapped as an `MTLTexture` (`newTextureWithDescriptor:iosurface:plane:`) and
     each needed view is stretched into the rect's active area of every tile. Sampling is
     clamped to that view's own texels, so the neighbouring view never bleeds in. The bars
     (batch) are the source, 1:1. Until the first result the batch layout writes the source into
     both tiles (flat). Nothing outside the rect is written.
3. **Hand over.** After `waitUntilCompleted`, `vk_conv_weave_host_done` points the helper at the
   shared buffers. `complete` then copies the snapshot into the stream's input slot (begin, copy,
   commit in one call, as on the Vulkan engines), reduces the lines on the CPU, and drops the pins.

**The IOSurface-wrap rule.** The texture's pixel format comes from the plan's conversion format
(`result_format`), never from `IOSurfaceGetPixelFormat`: an IOSurface MoltenVK created reports
pixel format 0 (the test asserts this). The format is always the UNORM alias, so an `_SRGB` result
is sampled raw and stays encoded, as the Vulkan path's raw copy keeps it. A result the glue could
not export as an IOSurface is woven flat. That covers a device-private result and a layered
(ARRAY) result, since an IOSurface has one layer. The line is `lift rect: stream 0x<id> failed:
export`.

**Sync ordering.** No `MTLSharedEvent` and no Vulkan semaphore are involved. The ordering rests on
three facts:

1. The glue publishes a result only after the Vulkan work writing it has completed. Every glue
   call waits for its own submit ("producer finished before hand-over", `XRT_CONV_SYNC_NONE`), and
   the #2030 mirror upload is a glue call too. So by the time `host_plan` pins a result, MoltenVK's
   command buffer that wrote the IOSurface has completed, and the Metal command buffer that reads
   it is committed later.
2. The pin keeps the provider from writing that ring slot again until `complete`. The engine
   calls `complete` only after its command buffer has completed.
3. The wrapped `MTLTexture` retains its IOSurface. Even a detach that frees the glue's image in
   the middle of a frame cannot free memory the command buffer still reads.

The snapshot travels the other way: Metal writes a shared buffer, and the CPU reads it after
`waitUntilCompleted`.

**Destroy while a weave is in flight.** On Linux stage B, the weave completes at the next submit.
If the client destroys the stream in between, the stream is only marked dead. The helper still
pins the result the in-flight weave reads, and the core's reaper skips a pinned stream (`dead &&
!pinned`). So the stream's images, which belong to the glue, outlive the weave.
`vk_conv_weave_complete` runs only after the fence. It drops the snapshot of the gone stream
(`failed: resolve`) and the pins, and only then are the images released. `tests_conv_vk_gpu`
destroys a stream with a weave in flight and checks that the glue's image count holds until
complete, then drops to 0. The test is also clean under ASan.

### Service log lines (greppable)

INFO lines, at most once per stream per change (never per frame). They need the service's log
level at `info`, for example `XRT_LOG=info`:

| When | Line |
|---|---|
| A snapshot is committed, first time and on each extent change | `lift rect: stream 0x<id> snapshot <w>x<h> (cap <n>)` (`<w>x<h>` after crop and cap; `<n>` = the long-edge cap, 0 = uncapped) |
| A result is first woven into the rect, and again on a result extent or view-count change | `lift rect: stream 0x<id> woven <W>x<H> views <n>` (`<W>x<H>` = the result image, views side by side) |
| A stage fails (logged once until it recovers) | `lift rect: stream 0x<id> failed: <stage>`, where `<stage>` is `resolve` (the stream is gone), `slot` (both input slots busy, latest wins), `copy` or `commit` |
| The rect names no core-routed stream of the connection | `lift rect: stream 0x<id> failed: resolve (not a core-routed stream of this connection); woven flat` |

Plus WARNs, once per change, as on Windows: `[lift] stream 0x<id>: letterbox crop ...`, and the
non-default `DXR_LIFT_MAX_INPUT_EDGE` / `DXR_LIFT_LETTERBOX=0` at attach. At attach the service
logs `[conv] Vulkan glue: lifted weave rects on`.

### Validated

- On a developer Mac (MoltenVK, the vk backend):
  - `tests_conv_vk_gpu "[weave_rects]"`: flat first, then a 1920x800 snapshot of a 2400x1000
    rect, the result in both tiles and nothing written outside the rect. The letterbox crop then
    settles to a 1920x640 snapshot and the bars are woven flat.
  - `weave_probe_vk_macos --lift` against a service with `DXR_CONV_SIM_CONVERSION_ENABLE=1`.
- On a developer Mac (the Metal backend, `DXR_WEAVE_MAC_BACKEND=metal`):
  - `tests_conv_vk_gpu "[weave_rects][metal]"` uses a headless Metal device and the MoltenVK
    glue. The first frame is flat, with the rect's corners exact in both tiles. The snapshot is
    1920x800. The result's IOSurface reports pixel format 0, and 98 sampled tile pixels match a
    CPU bilinear read of that IOSurface's views within 2/255. Nothing outside the rect is
    written. The letterbox crop settles to a 1920x640 snapshot with the bars flat. A stream
    destroyed with a Metal weave in flight keeps the glue's images until complete.
  - `weave_probe_vk_macos --lift` against the dev service with `DXR_WEAVE_MAC_BACKEND=metal
    DXR_CONV_SIM_CONVERSION_ENABLE=1`: rect A reads red (lifted), rect B cyan. The service logged
    `snapshot 320x180 (cap 1920)` and `woven 640x180 views 2`, with 90 submitted and 90 converted.
- The desktop-Linux engine shares the helper but has only been syntax-checked; it has not run.
- No POSIX weave engine draws a cursor, so `drawnLiftMask` stays 0. That is the spec's "not
  drawn" answer.
