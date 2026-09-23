# Cross-process dma-buf zero-copy spike: results (epic #1699, week 1)

**Question.** On a desktop Linux box with an Intel integrated GPU, can a dma-buf that one
process renders with GL/EGL (allocated through libgbm, as a browser GPU process does for a
canvas SharedImage) be imported by **another process** into Vulkan with its DRM format
modifier and **sampled correctly** at 60 Hz, without exhausting the fd table?

**Answer: YES**, for every modifier class the stack offers: LINEAR, X-tiled, 4-tiled and
the compressed `INTEL_4_TILED_LNL_CCS` (flat CCS). Every configuration sampled **bit-exact**
(tolerance 0, every pixel of every 1920×1080 frame checked) at a sustained 60.00 fps, in
both steady-state (one bo, fresh fds each frame) and worst-case (new bo each frame) modes,
with flat consumer fd tables. The same holds in the **return direction** (Vulkan allocates
and exports, GL imports and samples), which is the woven-output path. Two negative controls
prove the check is sensitive (wrong modifier → every pixel wrong; no release fence → 536 of
600 frames corrupted).

| modifier class | cross-process import + sampled read, bit-exact @ 60 Hz |
|---|---|
| `NONE_LINEAR` (0x0) | **yes** |
| `INTEL_X_TILED` (0x0100000000000001) | **yes** |
| `INTEL_4_TILED` (0x0100000000000009) | **yes** |
| `INTEL_4_TILED_LNL_CCS` (0x0100000000000010), compressed | **yes**. This is what gbm picks by default, and what the chromium-exact allocation picks |
| 2-plane CCS (`4_TILED_MTL_RC_CCS`, 0x010000000000000d) and `4_TILED_BMG_CCS` (0x0100000000000011) | not applicable: this GPU does not support them. iris refuses to allocate with `Unsupported modifier, resource creation failed.` |

## Environment

| | |
|---|---|
| OS / kernel | Ubuntu 26.04.1 LTS, Linux 7.0.0-31-generic |
| GPU | an Intel integrated GPU, PCI `8086:b090`, Xe2-class (its only CCS modifier is flat `LNL_CCS`) |
| render node / KMD | `/dev/dri/renderD128` (226:128), kernel driver **`xe`** |
| Vulkan (consumer) | driverName `Intel open-source Mesa driver`, driverInfo `Mesa 26.0.8-1ubuntu0.3`, driverID 6 (ANV), API 1.4.335 |
| GL (producer) | gbm backend `drm`; EGL 1.5 `Mesa Project` on `EGL_PLATFORM_GBM_KHR`, surfaceless + no-config context; `OpenGL ES 3.2 Mesa 26.0.8-1ubuntu0.3` (iris) |
| Compositor | GNOME Shell 50.1 (mutter), native Wayland; `zwp_linux_dmabuf_v1` v5 |
| Sync | `EGL_ANDROID_native_fence_sync` + `EGL_KHR_wait_sync` present; ANV SYNC_FD semaphores import=1 export=1 |

Everything ran offscreen: no window or surface was created on any display. `wl_query` binds
only the `zwp_linux_dmabuf_v1` global and reads the default feedback.

## Modifiers on offer

**ANV (`VkDrmFormatModifierPropertiesList2EXT`)** is identical for `ARGB8888` and `XRGB8888`
(`B8G8R8A8_UNORM`) and for `ABGR8888` (`R8G8B8A8_UNORM`). Every entry has 1 plane and
supports SAMPLED and COLOR_ATTACHMENT, and `vkGetPhysicalDeviceImageFormatProperties2`
accepts DMA_BUF import + SAMPLED for each:

```
0x0000000000000000 (NONE_LINEAR)            planes=1 SAMPLED=yes COLOR_ATTACHMENT=yes  dma-buf-import+SAMPLED: ok
0x0100000000000001 (INTEL_X_TILED)          planes=1 SAMPLED=yes COLOR_ATTACHMENT=yes  dma-buf-import+SAMPLED: ok
0x0100000000000009 (INTEL_4_TILED)          planes=1 SAMPLED=yes COLOR_ATTACHMENT=yes  dma-buf-import+SAMPLED: ok
0x0100000000000010 (INTEL_4_TILED_LNL_CCS)  planes=1 SAMPLED=yes COLOR_ATTACHMENT=yes  dma-buf-import+SAMPLED: ok
```

**EGL (`eglQueryDmaBufModifiersEXT`)** returns the same four for AR24 and AB24, none of them
external-only.

**mutter (`zwp_linux_dmabuf_v1` v5 default feedback)**: one main device (226:128 =
renderD128), one tranche (target renderD128, flags 0x0, 180 format entries). For each of
AR24, AB24 and XR24 it lists the same five modifiers, and v3 `modifier` events list the same set:

```
0x0000000000000000  NONE_LINEAR
0x0100000000000001  INTEL_X_TILED
0x0100000000000009  INTEL_4_TILED
0x0100000000000010  INTEL_4_TILED_LNL_CCS
0x00ffffffffffffff  NONE_INVALID          <- implicit-modifier entry; part of the list a client receives
```

So a Wayland GPU process that hands mutter's list to gbm gets `INTEL_4_TILED_LNL_CCS`, which
is compressed. The compressed case is therefore the default path, and it works.

## Matrix

Size 1920×1080 throughout. Mode A = one bo, fresh plane fds sent every frame (the steady
state), 600 frames. Mode B = a new bo every frame (worst case), 120 frames. Target 60 Hz
unless marked unpaced. Column meanings:
- **import**: CPU time for `vkCreateImage` + `vkGetMemoryFdPropertiesKHR` + dedicated
  `vkAllocateMemory(VkImportMemoryFdInfoKHR)` + bind + view + the acquire-semaphore import.
- **GPU sample**: timestamp delta for the sampled compute read of the full frame
  (`VkImageView` + nearest `VkSampler`, then `packUnorm4x8` into a host-visible buffer).
- **fds**: `/proc/self/fd` counts: consumer ready/peak/end, producer after-setup/peak/end.

**chromium-exact** rows reproduce the browser's Wayland allocation at its pinned tag: the v1
`gbm_bo_create_with_modifiers(dev,w,h,fourcc,mods,n)` with **no usage flags**, fed mutter's
main-device tranche list for the format **unfiltered** (so including `0x00ffffffffffffff`),
taken live from `wl_query --modlist=`. The `…no_modifiers_advertised` row covers the other
branch, `gbm_bo_create(SCANOUT|TEXTURING|RENDERING)`. Mesa's `gbm.h` has no
`GBM_BO_USE_TEXTURING` (it is a minigbm flag), so the harness passes SCANOUT|RENDERING.
Check how the browser build defines that flag against Mesa. RGBA_8888 maps to
`DRM_FORMAT_ABGR8888`, the important one; BGRA_8888 maps to `DRM_FORMAT_ARGB8888`. The
producer is **native Mesa GLES** standing in for the browser's ANGLE-GL-on-Mesa-EGL. Both
paths reach the same iris driver through the same EGL dma-buf import and native-fence
extensions, but this is not ANGLE itself.

**return_*** rows test the woven-output direction. The Vulkan side allocates an exportable
image (`VkExportMemoryAllocateInfo` DMA_BUF, dedicated). It uses either LINEAR or
`VkImageDrmFormatModifierListCreateInfoEXT` with mutter's list (with INVALID dropped,
because it is not a Vulkan modifier), and reads the chosen modifier back with
`vkGetImageDrmFormatModifierPropertiesEXT`. It then writes the frame, releases the image to
`VK_QUEUE_FAMILY_FOREIGN_EXT`, and sends a fresh `vkGetMemoryFdKHR` fd plus a SYNC_FD
acquire fence. The GL side imports that as an EGLImage texture, **samples it** into its own
RGBA8 FBO, returns a native-fence release fence, then does `glReadPixels` and checks every
pixel. For these rows the "import" column is the GL-side EGL import + fence wait, and the
next column is GL sample + readback.

| row | fourcc | modifier | planes | mode/frames | sampled bit-exact | fps | import µs avg/p99 | GPU sample µs avg/p99 | consumer fds ready/peak/end | producer fds setup/peak/end |
|---|---|---|---|---|---|---|---|---|---|---|
| ARGB8888_linear_A | AR24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 60.00 | 132/221 | 703/807 | 16/18/16 | 10/12/10 |
| ARGB8888_linear_B | AR24 | 0x0000000000000000 (NONE_LINEAR) | 1 | B/120 | YES | 60.00 | 142/255 | 704/815 | 16/18/16 | 10/12/10 |
| ARGB8888_auto_A | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 138/237 | 716/798 | 16/18/16 | 10/12/10 |
| ARGB8888_auto_B | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 162/227 | 746/800 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000001_A | AR24 | 0x0100000000000001 (INTEL_X_TILED) | 1 | A/600 | YES | 60.00 | 145/241 | 659/777 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000001_B | AR24 | 0x0100000000000001 (INTEL_X_TILED) | 1 | B/120 | YES | 60.00 | 147/224 | 662/759 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000009_A | AR24 | 0x0100000000000009 (INTEL_4_TILED) | 1 | A/600 | YES | 60.00 | 143/238 | 713/762 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000009_B | AR24 | 0x0100000000000009 (INTEL_4_TILED) | 1 | B/120 | YES | 60.00 | 172/826 | 720/787 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000010_A | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 141/252 | 759/795 | 16/18/16 | 10/12/10 |
| ARGB8888_0x0100000000000010_B | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 156/224 | 758/800 | 16/18/16 | 10/12/10 |
| ABGR8888_linear_A | AB24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 60.00 | 134/236 | 644/775 | 16/18/16 | 10/12/10 |
| ABGR8888_linear_B | AB24 | 0x0000000000000000 (NONE_LINEAR) | 1 | B/120 | YES | 60.00 | 150/242 | 602/740 | 16/18/16 | 10/12/10 |
| ABGR8888_auto_A | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 140/269 | 729/811 | 16/18/16 | 10/12/10 |
| ABGR8888_auto_B | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 166/246 | 724/811 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000001_A | AB24 | 0x0100000000000001 (INTEL_X_TILED) | 1 | A/600 | YES | 60.00 | 138/260 | 718/760 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000001_B | AB24 | 0x0100000000000001 (INTEL_X_TILED) | 1 | B/120 | YES | 60.00 | 146/237 | 736/764 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000009_A | AB24 | 0x0100000000000009 (INTEL_4_TILED) | 1 | A/600 | YES | 60.00 | 133/234 | 727/755 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000009_B | AB24 | 0x0100000000000009 (INTEL_4_TILED) | 1 | B/120 | YES | 60.00 | 144/275 | 729/766 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000010_A | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 135/232 | 774/800 | 16/18/16 | 10/12/10 |
| ABGR8888_0x0100000000000010_B | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 166/303 | 775/804 | 16/18/16 | 10/12/10 |
| chromium-exact_ABGR8888_A | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 137/241 | 776/804 | 16/18/16 | 10/12/10 |
| chromium-exact_ABGR8888_B | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 170/735 | 778/821 | 16/18/16 | 10/12/10 |
| chromium-exact_ARGB8888_A | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 150/249 | 774/797 | 16/18/16 | 10/12/10 |
| chromium-exact_ARGB8888_B | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | B/120 | YES | 60.00 | 169/256 | 775/828 | 16/18/16 | 10/12/10 |
| chromium-exact_ABGR8888_A_control_glFinish_nofence | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 130/240 | 774/875 | 16/17/16 | 10/11/10 |
| chromium-exact_ABGR8888_A_list_without_INVALID | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 140/235 | 775/804 | 16/18/16 | 10/12/10 |
| chromium-exact_ABGR8888_A_no_modifiers_advertised | AB24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 60.00 | 131/234 | 778/809 | 16/18/16 | 10/12/10 |
| return_ABGR8888_linear | AB24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 59.61 | GL import+acquire 52/165 | GL sample+readback 7738/8849 | vk 16/18/16 | gl 9/12/10 |
| return_ABGR8888_driverpick_from_wayland_list | AB24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 59.99 | GL import+acquire 65/217 | GL sample+readback 5784/6540 | vk 16/18/16 | gl 9/12/10 |
| return_ARGB8888_driverpick_from_wayland_list | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | GL import+acquire 78/227 | GL sample+readback 5708/6407 | vk 16/18/16 | gl 9/12/10 |
| ARGB8888_auto_rendering_A | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 132/233 | 774/800 | 16/18/16 | 10/12/10 |
| ARGB8888_linearusage_A | AR24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 60.00 | 131/243 | 767/830 | 16/18/16 | 10/12/10 |
| ARGB8888_nomodapi_A | AR24 | 0x0000000000000000 (NONE_LINEAR) | 1 | A/600 | YES | 60.00 | 132/220 | 766/808 | 16/18/16 | 10/12/10 |
| XRGB8888_auto_A | XR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 139/246 | 777/832 | 16/18/16 | 10/12/10 |
| ARGB8888_auto_A_unpaced | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 201.93 | 24/105 | 716/819 | 16/18/16 | 10/12/10 |
| ARGB8888_auto_A_cached | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | YES | 60.00 | 70/154 | 775/807 | 16/18/16 | 10/12/10 |
| NEG_wrong_modifier | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/60 | NO (60/60 frames bad) | 59.99 | 158/1967 | 730/838 | 16/18/16 | 10/12/10 |
| NEG_no_release_wait | AR24 | 0x0100000000000010 (INTEL_4_TILED_LNL_CCS) | 1 | A/600 | NO (536/600 frames bad) | 795.25 | 52/108 | 396/673 | 16/18/16 | 10/12/10 |
| PROBE_2plane_4_TILED_MTL_RC_CCS | - | - | - | A/0 | n/a: Unsupported modifier, resource creation failed.; chromium-exact allocation gbm_bo_create_with_modifiers [chromium-exact] FAILED (gbm returned NULL) | - | 0/0 | 0/0 | 16/16/16 | - |
| PROBE_4_TILED_BMG_CCS | - | - | - | A/0 | n/a: Unsupported modifier, resource creation failed.; chromium-exact allocation gbm_bo_create_with_modifiers [chromium-exact] FAILED (gbm returned NULL) | - | 0/0 | 0/0 | 16/16/16 | - |

Reproduce (from `contrib/spike/dmabuf-zero-copy/`, after `make`):

| row | command |
|---|---|
| ARGB8888_linear_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=linear --mode=A --frames=600` |
| ARGB8888_linear_B | `./run_pair.sh -- --format=ARGB8888 --modifiers=linear --mode=B --frames=120` |
| ARGB8888_auto_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600` |
| ARGB8888_auto_B | `./run_pair.sh -- --format=ARGB8888 --modifiers=auto --mode=B --frames=120` |
| ARGB8888_0x0100000000000001_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000001 --mode=A --frames=600` |
| ARGB8888_0x0100000000000001_B | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000001 --mode=B --frames=120` |
| ARGB8888_0x0100000000000009_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000009 --mode=A --frames=600` |
| ARGB8888_0x0100000000000009_B | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000009 --mode=B --frames=120` |
| ARGB8888_0x0100000000000010_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000010 --mode=A --frames=600` |
| ARGB8888_0x0100000000000010_B | `./run_pair.sh -- --format=ARGB8888 --modifiers=0x0100000000000010 --mode=B --frames=120` |
| ABGR8888_linear_A | `./run_pair.sh -- --format=ABGR8888 --modifiers=linear --mode=A --frames=600` |
| ABGR8888_linear_B | `./run_pair.sh -- --format=ABGR8888 --modifiers=linear --mode=B --frames=120` |
| ABGR8888_auto_A | `./run_pair.sh -- --format=ABGR8888 --modifiers=auto --mode=A --frames=600` |
| ABGR8888_auto_B | `./run_pair.sh -- --format=ABGR8888 --modifiers=auto --mode=B --frames=120` |
| ABGR8888_0x0100000000000001_A | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000001 --mode=A --frames=600` |
| ABGR8888_0x0100000000000001_B | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000001 --mode=B --frames=120` |
| ABGR8888_0x0100000000000009_A | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000009 --mode=A --frames=600` |
| ABGR8888_0x0100000000000009_B | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000009 --mode=B --frames=120` |
| ABGR8888_0x0100000000000010_A | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000010 --mode=A --frames=600` |
| ABGR8888_0x0100000000000010_B | `./run_pair.sh -- --format=ABGR8888 --modifiers=0x0100000000000010 --mode=B --frames=120` |
| chromium-exact_ABGR8888_A | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers="$(build/wl_query --modlist=ABGR8888)" --mode=A --frames=600` |
| chromium-exact_ABGR8888_B | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers="$(build/wl_query --modlist=ABGR8888)" --mode=B --frames=120` |
| chromium-exact_ARGB8888_A | `./run_pair.sh -- --alloc=chromium --format=ARGB8888 --modifiers="$(build/wl_query --modlist=ARGB8888)" --mode=A --frames=600` |
| chromium-exact_ARGB8888_B | `./run_pair.sh -- --alloc=chromium --format=ARGB8888 --modifiers="$(build/wl_query --modlist=ARGB8888)" --mode=B --frames=120` |
| chromium-exact_ABGR8888_A_control_glFinish_nofence | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers="$(build/wl_query --modlist=ABGR8888)" --mode=A --frames=600 --no-fence` |
| chromium-exact_ABGR8888_A_list_without_INVALID | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers=$(echo ""$(build/wl_query --modlist=ABGR8888)"" | sed 's/,0x00ffffffffffffff//') --mode=A --frames=600` |
| chromium-exact_ABGR8888_A_no_modifiers_advertised | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers=none --mode=A --frames=600` |
| return_ABGR8888_linear | `./run_pair.sh --return-path=linear --format=ABGR8888 --frames=600 -- --return-path` |
| return_ABGR8888_driverpick_from_wayland_list | `./run_pair.sh --return-path=list:"$(build/wl_query --modlist=ABGR8888)" --format=ABGR8888 --frames=600 -- --return-path` |
| return_ARGB8888_driverpick_from_wayland_list | `./run_pair.sh --return-path=list:"$(build/wl_query --modlist=ARGB8888)" --format=ARGB8888 --frames=600 -- --return-path` |
| ARGB8888_auto_rendering_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=auto --usage=rendering --mode=A --frames=600` |
| ARGB8888_linearusage_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=none --usage=rendering,linear --mode=A --frames=600` |
| ARGB8888_nomodapi_A | `./run_pair.sh -- --format=ARGB8888 --modifiers=none --usage=rendering,scanout --mode=A --frames=600` |
| XRGB8888_auto_A | `./run_pair.sh -- --format=XRGB8888 --modifiers=auto --mode=A --frames=600` |
| ARGB8888_auto_A_unpaced | `./run_pair.sh -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600 --fps=0` |
| ARGB8888_auto_A_cached | `./run_pair.sh --cache-imports -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600` |
| NEG_wrong_modifier | `./run_pair.sh --force-modifier=0x0100000000000009 -- --format=ARGB8888 --modifiers=0x0100000000000010 --mode=A --frames=60` |
| NEG_no_release_wait | `./run_pair.sh --sparse-verify -- --format=ARGB8888 --modifiers=auto --mode=A --frames=600 --fps=0 --no-release-wait` |
| PROBE_2plane_4_TILED_MTL_RC_CCS | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers=0x010000000000000d --frames=10` |
| PROBE_4_TILED_BMG_CCS | `./run_pair.sh -- --alloc=chromium --format=ABGR8888 --modifiers=0x0100000000000011 --frames=10` |

### Soak and A/B runs (`--sparse-verify`: every 7th pixel on both axes; all 0 mismatches)

| run | fps | consumer fds ready/peak/end | producer fds setup/peak/end |
|---|---|---|---|
| chromium-exact ABGR8888, Mode A, 20000 frames, unpaced | 888.95 | 16/18/16 | 10/12/10 |
| chromium-exact ABGR8888, Mode B, 3600 frames (60 s) @ 60 Hz | 60.00 | 16/18/16 | 10/12/10 |
| chromium-exact ABGR8888, Mode B, 20000 frames, unpaced | 1581.43 | 16/18/16 | **10/155/153**, see the fd note below |
| consumer on llvmpipe (`--cpu`), LINEAR, 60 frames, full verify | 20.82 | 10/12/10 | 10/12/10 |

Reproduce, for example:
`./run_pair.sh --sparse-verify -- --alloc=chromium --format=ABGR8888 --modifiers="$(build/wl_query --modlist=ABGR8888)" --mode=B --frames=3600`.

## What the numbers say, and what was surprising

1. **Compression does not break zero-copy here, provided the modifier travels with the
   buffer.** gbm picks `INTEL_4_TILED_LNL_CCS` whenever it appears in the list, and it does
   appear in mutter's list and ANV's. Cross-process import and sampling of that buffer is
   bit-exact. The `NEG_wrong_modifier` control imports the same CCS buffer as plain
   `INTEL_4_TILED` and gets **every pixel wrong on every frame**. That confirms the data
   really is compressed, and that honoring the modifier exactly is what makes it work.
   Xe2's flat CCS keeps compression metadata out of any separate plane, so these buffers
   are all single-plane.
2. **Multi-plane modifiers do not exist on this GPU.** No format here offers a modifier with
   `drmFormatModifierPlaneCount > 1`, and iris refuses the older 2-plane CCS modifiers
   outright. The consumer does handle N memory planes in the non-disjoint layout (one
   dma-buf, one dedicated import, per-plane `VkSubresourceLayout` from the message; it
   checks that every plane fd is the same dma-buf by inode, and the `planes_share_dmabuf`
   field reports that check). That path could not be exercised on this hardware. It still
   needs a run on an older Intel part or an AMD DCC part. Disjoint (multi-bo) import is not
   implemented.
3. **The release fence is mandatory: implicit sync did not save us.** `NEG_no_release_wait`
   has the GL writer skip waiting on the consumer's SYNC_FD release fence, and 536 of 600
   frames sampled a half-overwritten buffer. Carrying the release fence back (SYNC_FD
   exported from a `VkSemaphore`, imported as `EGL_SYNC_NATIVE_FENCE_ANDROID` and GPU-waited
   with `eglWaitSyncKHR`) makes it bit-exact. The acquire direction works the same way:
   `eglDupNativeFenceFDANDROID` produces the fd, which is imported as a temporary SYNC_FD
   `VkSemaphore`. The `glFinish`/no-fence control also passes, but it CPU-stalls the producer.
4. **fd accounting is clean on the consumer.** Per the Vulkan spec
   (`VkImportMemoryFdInfoKHR`), a successful import transfers ownership of the fd for every
   fd handle type, DMA_BUF included. The consumer therefore does not close the imported
   plane-0 fd. It closes the extra plane fds and every fd from a failed import. Its table
   is 16/18/16 in every row: two fds in flight (plane + fence), none retained. The same
   holds for SYNC_FD semaphore imports.
5. **Producer-side fd retention under unpaced churn.** In Mode B unpaced, the *GL* process
   ended holding up to about 150 extra `/dmabuf:` fds. They drain but are not reaped
   promptly. Cause: on the `xe` KMD, iris keeps a dup'd prime fd on every external bo for
   implicit-sync `DMA_BUF_IOCTL_{EXPORT,IMPORT}_SYNC_FILE` (`iris_bufmgr.c`:
   `needs_prime_fd()` returns true for `INTEL_KMD_TYPE_XE`), and that fd is closed only when
   the zombie bo is finally freed. At 60 Hz the count is flat (10/12/10 over 3600 frames of
   Mode B), and Mode A is flat even unpaced. This is a browser GPU-process property, not a
   runtime one. Pooled SharedImages avoid it. Keep an eye on it if the browser ever churns
   buffers at high rates.
6. **Timings.** CPU import cost is 130–170 µs average, p99 about 250 µs (occasional first-frame
   outliers near 2 ms). With `--cache-imports` (re-sampling a cached `VkImage` when the same
   bo comes back, and only importing the fence) it is about 70 µs. The GPU sampled read of a
   full 1080p frame is about 0.6–0.8 ms at the light-load clocks a 60 Hz loop leaves the GPU
   in, with no meaningful difference between LINEAR, tiled and CCS at this load. Latency from
   message receipt to sample complete includes waiting on the producer's own render through
   the acquire fence. It was 1.5–4.5 ms and varied with GPU clock state, so treat it as an
   upper bound, not a cost. Unpaced, the pair sustained about 200 fps with full per-pixel
   verification and about 900–1500 fps with sparse verification.
7. **The chromium-exact list includes `DRM_FORMAT_MOD_INVALID`, and that is harmless.** v1
   `gbm_bo_create_with_modifiers` with `…,0x00ffffffffffffff` in the list picks
   `LNL_CCS`, exactly as it does with INVALID stripped. When no modifiers are advertised,
   `gbm_bo_create` gives LINEAR, and that works too.
8. **Minor:** the Vulkan loader also initialises lavapipe, so the consumer carries
   `/dev/udmabuf` and a few memfds it never uses. A runtime can drop them by filtering ICDs.

## Harness

`contrib/spike/dmabuf-zero-copy/` is standalone. It is not part of the runtime's CMake build
and uses no runtime code, plug-in or `XRT_*` variable.

- `producer.c`: the GL/EGL side. It opens renderD128 and creates a gbm device and a bo
  (`_with_modifiers2` → `_with_modifiers` → `gbm_bo_create`, or `--alloc=chromium` for the
  exact browser call). It creates an EGL display on the gbm platform with a surfaceless GLES
  3 context, imports the bo as an EGLImage with an explicit modifier, and attaches it as an
  FBO renderbuffer. Each frame it draws a frame-dependent pattern (border, gradient, and a
  32×32 checker encoding `frame % 251`), exports a native-fence sync_fd, and sends plane
  fds, strides, offsets, modifier, format, size, frame and fence over `SOCK_SEQPACKET` +
  `SCM_RIGHTS`. It then GPU-waits the release fence before touching the bo again.
  `--return-path` runs the GL importer for the woven-output direction.
- `consumer.c`: headless Vulkan. It picks the physical device whose
  `VK_EXT_physical_device_drm` render node matches (`--cpu` picks llvmpipe) and enables
  `external_memory_fd`, `external_memory_dma_buf`, `image_drm_format_modifier`,
  `queue_family_foreign`, `external_semaphore_fd` and `external_fence_fd`. Each frame it
  imports with explicit plane layouts, acquires from `VK_QUEUE_FAMILY_FOREIGN_EXT`
  (GENERAL → SHADER_READ_ONLY), waits the acquire semaphore, and samples in `sample.comp`.
  It then releases back to FOREIGN, exports and sends a SYNC_FD release fence straight
  away (pipelined), and verifies every pixel. `--list` prints the modifier table.
  `--return-path=linear|list:…` runs the Vulkan exporter. `--force-modifier=` is the
  negative control.
- `wl_query.c`: prints mutter's dma-buf feedback. `--modlist=FOURCC` prints only the
  main-device list, comma-separated, for feeding to the producer.
- `run_pair.sh [consumer args] -- [producer args]` runs one pair on a private socket.
  `run_matrix.sh` runs everything above into `build/logs/`.

Build with `make` (needs libdrm-dev, libegl-dev, libgles-dev, libvulkan-dev, libwayland-dev,
wayland-protocols, glslang-tools). **`libgbm-dev` was not installed on the test box**, so
`third_party/gbm.h` is Mesa 26.0.8's `src/gbm/main/gbm.h`, verbatim under its MIT
licence, and the Makefile links through a local `build/libgbm.so -> libgbm.so.1` symlink.
