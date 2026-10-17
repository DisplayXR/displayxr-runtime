// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `--lift` arm of the Vulkan handle apps: XR_DXR_lift spec 4 over CPU
 *         images from an in-process session (the POSIX v1 route).
 *
 * Header-only, used by cube_handle_vk_macos and cube_handle_vk_linux. With
 * `--lift` (or DISPLAYXR_LIFT_PROBE=1) the app enables XR_DXR_lift and, once
 * its session runs:
 *  - enumerates the lift tasks (xrEnumerateLiftTasksDXR) until VIEWS_MONO is
 *    READY with a CPU image kind on this session, logging every change;
 *  - creates a VIEWS_MONO stream (2 views, CPU input, CPU export);
 *    DISPLAYXR_LIFT_PROBE_SIZE=WxH sets the input size (default 640x360);
 *  - every DISPLAYXR_LIFT_PROBE_EVERY frames (default 10) submits a synthetic
 *    RGBA8 frame (a bright vertical bar at the centre over a dim gradient)
 *    and every frame tries to acquire a result;
 *  - measures, on acquired results, where the bar landed in each view: the
 *    difference is the view shift the provider synthesised;
 *  - logs submitted / acquired / not-ready / failed counts and the task-state
 *    transitions on exit. DISPLAYXR_LIFT_PROBE_FRAMES=N asks the session to
 *    exit after N frames of streaming (0 = run until closed).
 *
 * `--lift-gpu` (macOS; or DISPLAYXR_LIFT_PROBE=gpu) is the zero-copy arm of
 * ADR-053 P5 step E: the frame is uploaded ONCE into a VkImage the app made
 * over a global IOSurface (VK_EXT_metal_objects), every submit hands that
 * IOSurface in (kind IOSURFACE, CPU_COMPLETE), and the stream is created
 * with exportKind IOSURFACE: the result arrives as an IOSurface the app
 * imports into its own VkImage (once per allocation). The probe logs the
 * per-frame copies the GPU arm avoids, the time spent in
 * xrSubmitLiftFrameDXR and the conversion latency, for comparison with a
 * `--lift` (CPU) run. Needs the in-process session's imageKinds / exportKinds
 * to list IOSURFACE; otherwise it says why and runs the CPU arm.
 *
 * Every line starts with "[lift-probe]" so a run can be grepped.
 */

#pragma once

#include <openxr/XR_DXR_lift.h>
#include <openxr/openxr.h>

// The GPU arm: macOS, in a translation unit that included <vulkan/vulkan.h>.
#if defined(__APPLE__) && defined(VK_HEADER_VERSION)
#define DXR_LIFT_PROBE_GPU 1
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <vulkan/vulkan_metal.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dxr_test {

class LiftCpuProbe {
public:
  //! `--lift` / `--lift-gpu` on the command line or DISPLAYXR_LIFT_PROBE=1 / gpu.
  static bool Requested(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--lift") == 0 || strcmp(argv[i], "--lift-gpu") == 0) {
        return true;
      }
    }
    const char *e = getenv("DISPLAYXR_LIFT_PROBE");
    return e != nullptr && (e[0] == '1' || strcmp(e, "gpu") == 0);
  }

  //! The zero-copy arm was asked for (`--lift-gpu` / DISPLAYXR_LIFT_PROBE=gpu).
  static bool RequestedGpu(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--lift-gpu") == 0) {
        return true;
      }
    }
    const char *e = getenv("DISPLAYXR_LIFT_PROBE");
    return e != nullptr && strcmp(e, "gpu") == 0;
  }

  bool enabled = false;
  bool gpu = false; //!< the zero-copy arm (macOS)

#if defined(DXR_LIFT_PROBE_GPU)
  //! The device extensions the GPU arm needs (add them when available).
  static const char *const *GpuDeviceExtensions(uint32_t *count) {
    static const char *const k[] = {"VK_EXT_metal_objects",
                                    "VK_EXT_external_memory_metal"};
    *count = 2;
    return k;
  }

  //! After the app created its device (the session's graphics binding).
  void SetVulkan(VkPhysicalDevice pd, VkDevice dev, VkQueue queue,
                 uint32_t family) {
    if (!enabled || !gpu) {
      return;
    }
    pd_ = pd;
    dev_ = dev;
    queue_ = queue;
    family_ = family;
    export_ = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(
        dev, "vkExportMetalObjectsEXT");
    mem_props_ = (PFN_vkGetMemoryMetalHandlePropertiesEXT)vkGetDeviceProcAddr(
        dev, "vkGetMemoryMetalHandlePropertiesEXT");
    if (export_ == nullptr || mem_props_ == nullptr) {
      printf("[lift-probe] GPU arm: VK_EXT_metal_objects / "
             "VK_EXT_external_memory_metal not enabled on the app's device: "
             "CPU arm\n");
      gpu = false;
    }
  }
#endif

  //! After xrCreateInstance (with XR_DXR_lift enabled).
  void OnInstance(XrInstance instance) {
    if (!enabled) {
      return;
    }
    bool ok =
        Get(instance, "xrEnumerateLiftTasksDXR",
            (PFN_xrVoidFunction *)&enumerate_) &&
        Get(instance, "xrCreateLiftStreamDXR",
            (PFN_xrVoidFunction *)&create_) &&
        Get(instance, "xrDestroyLiftStreamDXR",
            (PFN_xrVoidFunction *)&destroy_) &&
        Get(instance, "xrSubmitLiftFrameDXR", (PFN_xrVoidFunction *)&submit_) &&
        Get(instance, "xrAcquireLiftResultDXR",
            (PFN_xrVoidFunction *)&acquire_);
    (void)Get(instance, "xrGetLiftStreamStatsDXR",
              (PFN_xrVoidFunction *)&stats_);
    if (!ok) {
      printf("[lift-probe] XR_DXR_lift entry points missing: probe off\n");
      enabled = false;
      return;
    }
    const char *e = getenv("DISPLAYXR_LIFT_PROBE_EVERY");
    every_ = e != nullptr && atoi(e) > 0 ? (uint32_t)atoi(e) : 10u;
    e = getenv("DISPLAYXR_LIFT_PROBE_FRAMES");
    budget_ = e != nullptr && atoi(e) > 0 ? (uint32_t)atoi(e) : 0u;
    e = getenv("DISPLAYXR_LIFT_PROBE_SIZE");
    unsigned w = 0, h = 0;
    if (e != nullptr && sscanf(e, "%ux%u", &w, &h) == 2 && w >= 16 && h >= 16 &&
        w <= 8192 && h <= 8192) {
      kW = w;
      kH = h;
    }
    printf(
        "[lift-probe] armed: %ux%u RGBA8 input, submit every %u frames, %s\n",
        kW, kH, every_, budget_ != 0 ? "bounded run" : "until closed");
    MakeFrame();
  }

  //! From the app's xrPollEvent loop.
  void OnEvent(const XrEventDataBuffer &ev) {
    if (!enabled) {
      return;
    }
    if (ev.type == XR_TYPE_EVENT_DATA_LIFT_TASK_STATE_CHANGED_DXR) {
      const auto *e =
          reinterpret_cast<const XrEventDataLiftTaskStateChangedDXR *>(&ev);
      transitions_++;
      printf("[lift-probe] event: task %d state %s -> %s (reason %d, progress "
             "%.2f)\n",
             (int)e->task, StateName(e->previousState), StateName(e->state),
             (int)e->failureReason, (double)e->progress);
    } else if (ev.type == XR_TYPE_EVENT_DATA_LIFT_PROVIDERS_CHANGED_DXR) {
      printf("[lift-probe] event: providers changed\n");
    }
  }

  //! Once per frame while the session runs. True = the bounded run is over.
  bool Tick(XrSession session) {
    if (!enabled) {
      return false;
    }
    frame_++;
    if (stream_ == XR_NULL_HANDLE) {
      if (frame_ % 30 == 1) {
        TryCreate(session);
      }
      return false;
    }
    streamed_++;
    if (streamed_ % every_ == 1 || every_ == 1) {
      Submit();
    }
    Acquire();
    return budget_ != 0 && streamed_ >= budget_;
  }

  //! Before xrDestroySession.
  void Shutdown() {
    if (!enabled || done_) {
      return;
    }
    done_ = true;
    if (stream_ != XR_NULL_HANDLE && stats_ != nullptr) {
      XrLiftStreamStatsDXR st = {XR_TYPE_LIFT_STREAM_STATS_DXR};
      if (XR_SUCCEEDED(stats_(stream_, &st))) {
        printf("[lift-probe] stream stats: submitted %llu converted %llu "
               "dropped %llu failed %llu, "
               "latency last %.2f ms avg %.2f ms, %.1f results/s\n",
               (unsigned long long)st.framesSubmitted,
               (unsigned long long)st.framesConverted,
               (unsigned long long)st.framesDropped,
               (unsigned long long)st.framesFailed,
               (double)st.latencyLast / 1e6, (double)st.latencyAverage / 1e6,
               (double)st.conversionRate);
      }
    }
    if (stream_ != XR_NULL_HANDLE) {
      destroy_(stream_);
      stream_ = XR_NULL_HANDLE;
    }
    const double submit_us =
        submitted_ > 0 ? submit_ns_ / 1000.0 / (double)submitted_ : 0.0;
    const double lat_ms =
        lat_samples_ > 0 ? lat_ns_ / 1e6 / (double)lat_samples_ : 0.0;
    const uint64_t in_bytes = (uint64_t)kW * kH * 4u;
    if (gpu_live_) {
      printf("[lift-probe] GPU arm: per submit %.1f us in xrSubmitLiftFrameDXR, "
             "conversion latency avg %.2f ms over %u results; avoided per frame "
             "vs the CPU arm: the app's GPU->host readback + the runtime's CPU "
             "staging copy + the service's section->slot copy (%llu B each) on "
             "input, the service's result readback (%llu B) on output; export "
             "imports %u, export byte-exact checks %u/%u\n",
             submit_us, lat_ms, lat_samples_, (unsigned long long)in_bytes,
             (unsigned long long)last_out_bytes_, export_imports_,
             export_match_, export_checks_);
    } else {
      printf("[lift-probe] CPU arm: per submit %.1f us in xrSubmitLiftFrameDXR "
             "(incl. the runtime's %llu B staging copy), conversion latency avg "
             "%.2f ms over %u results\n",
             submit_us, (unsigned long long)in_bytes, lat_ms, lat_samples_);
    }
#if defined(DXR_LIFT_PROBE_GPU)
    GpuRelease();
#endif
    printf("[lift-probe] SUMMARY: submitted=%u submit_failed=%u acquired=%u "
           "not_ready=%u acquire_failed=%u "
           "skipped=%u transitions=%u shift_px(last)=%d shift_samples=%u\n",
           submitted_, submit_failed_, acquired_, not_ready_, acquire_failed_,
           skipped_, transitions_, last_shift_, shift_samples_);
  }

private:
  uint32_t kW = 640, kH = 360; //!< input size

  static bool Get(XrInstance instance, const char *name,
                  PFN_xrVoidFunction *out) {
    *out = nullptr;
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, out)) &&
           *out != nullptr;
  }

  static const char *StateName(XrLiftTaskStateDXR s) {
    static const char *k[] = {"UNKNOWN",     "READY",        "WARMING",
                              "ABSENT",      "INCOMPATIBLE", "UNLICENSED",
                              "FAILED",      "DISABLED",     "INSTALLED",
                              "DOWNLOADING", "EVICTED"};
    return (int)s >= 0 && (int)s <= 10 ? k[(int)s] : "?";
  }

  void MakeFrame() {
    pixels_.assign((size_t)kW * kH * 4u, 0);
    for (uint32_t y = 0; y < kH; y++) {
      for (uint32_t x = 0; x < kW; x++) {
        uint8_t *p = &pixels_[((size_t)y * kW + x) * 4u];
        const bool bar = x >= kW / 2 - 2 && x < kW / 2 + 2;
        p[0] = bar ? 250 : (uint8_t)(20 + 60 * x / kW);
        p[1] = bar ? 250 : (uint8_t)(20 + 60 * y / kH);
        p[2] = bar ? 250 : 40;
        p[3] = 255;
      }
    }
  }

  void TryCreate(XrSession session) {
    uint32_t n = 0;
    XrResult r = enumerate_(session, 0, &n, nullptr);
    std::vector<XrLiftTaskPropertiesDXR> tasks(
        n, {XR_TYPE_LIFT_TASK_PROPERTIES_DXR});
    if (XR_SUCCEEDED(r) && n > 0) {
      r = enumerate_(session, n, &n, tasks.data());
    }
    if (XR_FAILED(r)) {
      if (r != last_enum_result_) {
        printf("[lift-probe] xrEnumerateLiftTasksDXR failed: %d\n", (int)r);
      }
      last_enum_result_ = r;
      return;
    }
    last_enum_result_ = r;
    const XrLiftTaskPropertiesDXR *views = nullptr;
    char sig[256] = {0};
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
      at +=
          (size_t)snprintf(sig + at, sizeof(sig) - at, "%d:%s/in%llx/out%llx ",
                           (int)tasks[i].task, StateName(tasks[i].state),
                           (unsigned long long)tasks[i].imageKinds,
                           (unsigned long long)tasks[i].exportKinds);
      if (at >= sizeof(sig)) {
        at = sizeof(sig) - 1;
      }
      if (tasks[i].task == XR_LIFT_TASK_VIEWS_MONO_DXR) {
        views = &tasks[i];
      }
    }
    if (strcmp(sig, last_sig_) != 0) {
      printf("[lift-probe] tasks (%u): %s\n", n,
             n > 0 ? sig : "(none: no service / no provider yet)");
      snprintf(last_sig_, sizeof(last_sig_), "%s", sig);
    }
    const uint64_t cpu = 1ull << XR_LIFT_IMAGE_KIND_CPU_DXR;
    if (views == nullptr || views->state != XR_LIFT_TASK_STATE_READY_DXR ||
        (views->imageKinds & cpu) == 0 || (views->exportKinds & cpu) == 0) {
      return;
    }
#if defined(DXR_LIFT_PROBE_GPU)
    const uint64_t ios = 1ull << XR_LIFT_IMAGE_KIND_IOSURFACE_DXR;
    if (gpu && ((views->imageKinds & ios) == 0 || (views->exportKinds & ios) == 0)) {
      printf("[lift-probe] GPU arm: IOSURFACE not in this session's imageKinds "
             "0x%llx / exportKinds 0x%llx (service without the Vulkan glue's "
             "transport?): CPU arm\n",
             (unsigned long long)views->imageKinds,
             (unsigned long long)views->exportKinds);
      gpu = false;
    }
    if (gpu && !GpuMakeInput()) {
      printf("[lift-probe] GPU arm: could not make the IOSurface input: CPU "
             "arm\n");
      gpu = false;
    }
    gpu_live_ = gpu;
#endif
    XrLiftTaskStreamCreateInfoDXR task = {
        XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR};
    task.task = XR_LIFT_TASK_VIEWS_MONO_DXR;
    task.inputKind = XR_LIFT_INPUT_KIND_MONO_DXR;
    task.viewCount = 2;
    task.exportKind = gpu_live_ ? XR_LIFT_IMAGE_KIND_IOSURFACE_DXR
                                : XR_LIFT_IMAGE_KIND_CPU_DXR;
    XrLiftStreamCreateInfoDXR ci = {XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR, &task};
    ci.contentHint = XR_LIFT_CONTENT_HINT_VIDEO_DXR;
    ci.inputScale = 1.0f;
    r = create_(session, &ci, &stream_);
    printf("[lift-probe] xrCreateLiftStreamDXR(VIEWS_MONO, 2 views, %s "
           "in/out): %d\n",
           gpu_live_ ? "IOSURFACE" : "CPU", (int)r);
    if (XR_FAILED(r)) {
      stream_ = XR_NULL_HANDLE;
    }
  }

  void Submit() {
    XrLiftImageDXR im = {XR_TYPE_LIFT_IMAGE_DXR};
    im.role = XR_LIFT_IMAGE_ROLE_COLOR_DXR;
    im.kind = XR_LIFT_IMAGE_KIND_CPU_DXR;
    im.cpuBytes = pixels_.data();
    im.cpuRowPitch = kW * 4u;
#if defined(DXR_LIFT_PROBE_GPU)
    if (gpu_live_) {
      // The IOSurface behind the app's VkImage: no bytes cross on the CPU.
      im.kind = XR_LIFT_IMAGE_KIND_IOSURFACE_DXR;
      im.handle = (void *)in_surf_;
      im.cpuBytes = nullptr;
      im.cpuRowPitch = 0;
    }
#endif
    im.format = XR_LIFT_PIXEL_FORMAT_RGBA8_UNORM_DXR;
    im.region = {{0, 0}, {(int32_t)kW, (int32_t)kH}};
    im.syncKind = XR_LIFT_SYNC_KIND_CPU_COMPLETE_DXR;
    XrLiftFrameImagesDXR fi = {XR_TYPE_LIFT_FRAME_IMAGES_DXR};
    fi.imageCount = 1;
    fi.images = &im;
    XrLiftFrameSubmitInfoDXR si = {XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR, &fi};
    si.extent = {(int32_t)kW, (int32_t)kH};
    si.sourceTime = (XrTime)streamed_;
    uint64_t id = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const XrResult r = submit_(stream_, &si, &id);
    const auto t1 = std::chrono::steady_clock::now();
    if (XR_SUCCEEDED(r)) {
      submit_ns_ +=
          (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count();
      submitted_++;
    } else {
      submit_failed_++;
      if (submit_failed_ <= 3) {
        printf("[lift-probe] xrSubmitLiftFrameDXR failed: %d\n", (int)r);
      }
    }
  }

  void Acquire() {
    XrLiftViewsInfoDXR vi = {XR_TYPE_LIFT_VIEWS_INFO_DXR};
    XrLiftResultExportDXR ex = {XR_TYPE_LIFT_RESULT_EXPORT_DXR, &vi};
    XrLiftResultDXR res = {XR_TYPE_LIFT_RESULT_DXR, &ex};
    const XrResult r = acquire_(stream_, &res);
    if (r == XR_LIFT_NOT_READY_DXR) {
      not_ready_++;
      return;
    }
    if (XR_FAILED(r)) {
      acquire_failed_++;
      if (acquire_failed_ <= 3) {
        printf("[lift-probe] xrAcquireLiftResultDXR failed: %d\n", (int)r);
      }
      return;
    }
    acquired_++;
    lat_ns_ += (double)res.latency;
    lat_samples_++;
    const uint8_t *px = static_cast<const uint8_t *>(res.outputTexture);
    const uint32_t w = (uint32_t)res.extent.width,
                   h = (uint32_t)res.extent.height;
    const uint32_t views = res.viewCount != 0 ? res.viewCount : 1;
    last_out_bytes_ = (uint64_t)w * h * 4u;
#if defined(DXR_LIFT_PROBE_GPU)
    std::vector<uint8_t> gpu_rows;
    if (gpu_live_) {
      // The export IOSurface (handed over on the first acquire and on each
      // reallocation): imported into the app's own VkImage; its middle row is
      // read back through Vulkan for the measurement below.
      if (res.outputTexture != nullptr) {
        GpuTakeExport((IOSurfaceRef)res.outputTexture, w, h);
      }
      px = nullptr;
      if (ex.kind == XR_LIFT_IMAGE_KIND_IOSURFACE_DXR && out_img_ != VK_NULL_HANDLE &&
          out_w_ == w && out_h_ == h && GpuReadRow(h / 2, gpu_rows)) {
        // The row's bytes stand in for the whole image at the row offset.
        gpu_frame_.assign((size_t)w * h * 4u, 0);
        memcpy(&gpu_frame_[(size_t)(h / 2) * w * 4u], gpu_rows.data(),
               gpu_rows.size());
        px = gpu_frame_.data();
      }
    }
    if (gpu_live_ ? px == nullptr || w < views || h == 0
                  : (px == nullptr || ex.kind != XR_LIFT_IMAGE_KIND_CPU_DXR ||
                     w < views || h == 0)) {
#else
    if (px == nullptr || ex.kind != XR_LIFT_IMAGE_KIND_CPU_DXR || w < views ||
        h == 0) {
#endif
      skipped_++;
      if (skipped_ <= 3) {
        printf("[lift-probe] result %llu: no CPU bytes (kind %d, %ux%u)\n",
               (unsigned long long)res.frameId, (int)ex.kind, w, h);
      }
      return;
    }
    // Where is the bar in each view (row = the middle line)? Tightly packed
    // RGBA8.
    const uint32_t vw = w / views;
    const uint8_t *row = px + (size_t)(h / 2) * w * 4u;
    int pos[2] = {-1, -1};
    for (uint32_t v = 0; v < views && v < 2; v++) {
      uint32_t best = 0;
      for (uint32_t x = 0; x < vw; x++) {
        const uint8_t *p = row + ((size_t)v * vw + x) * 4u;
        const uint32_t s = (uint32_t)p[0] + p[1] + p[2];
        if (s > best) {
          best = s;
          pos[v] = (int)x;
        }
      }
    }
    if (views >= 2 && pos[0] >= 0 && pos[1] >= 0) {
      last_shift_ = pos[1] - pos[0];
      shift_samples_++;
    }
    if (acquired_ <= 3 || acquired_ % 30 == 0) {
      printf("[lift-probe] result %llu: %ux%u, %u views (%ux%u each), fmt %d, "
             "latency %.2f ms, bar at "
             "x=%d / x=%d (input x=%u): shift %d px\n",
             (unsigned long long)res.frameId, w, h, views, vi.viewExtent.width,
             vi.viewExtent.height, (int)ex.format, (double)res.latency / 1e6,
             pos[0], pos[1], kW / 2, last_shift_);
    }
  }

#if defined(DXR_LIFT_PROBE_GPU)
  //! A global IOSurface (the transport carries its ID) as an app VkImage:
  //! VkImportMetalIOSurfaceInfoEXT, then the MTLTexture memory import.
  bool GpuImport(IOSurfaceRef surf, uint32_t w, uint32_t h, VkImage *img,
                 VkDeviceMemory *mem) {
    VkExportMetalObjectCreateInfoEXT ex = {
        VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT};
    ex.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
    VkImportMetalIOSurfaceInfoEXT imp = {
        VK_STRUCTURE_TYPE_IMPORT_METAL_IO_SURFACE_INFO_EXT};
    imp.pNext = &ex;
    imp.ioSurface = surf;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &imp;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev_, &ici, nullptr, img) != VK_SUCCESS) {
      *img = VK_NULL_HANDLE;
      return false;
    }
    VkExportMetalTextureInfoEXT et = {
        VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    et.image = *img;
    et.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT eo = {
        VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
    eo.pNext = &et;
    export_(dev_, &eo);
    VkMemoryMetalHandlePropertiesEXT mp = {
        VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT};
    uint32_t type = 0;
    if (et.mtlTexture == nullptr ||
        mem_props_(dev_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
                   et.mtlTexture, &mp) != VK_SUCCESS) {
      return false;
    }
    while (type < 32 && (mp.memoryTypeBits & (1u << type)) == 0) {
      type++;
    }
    VkImportMemoryMetalHandleInfoEXT im = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT;
    im.handle = et.mtlTexture;
    VkMemoryDedicatedAllocateInfo ded = {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.pNext = &im;
    ded.image = *img;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &ded;
    ai.allocationSize = 0;
    ai.memoryTypeIndex = type;
    return type < 32 && vkAllocateMemory(dev_, &ai, nullptr, mem) == VK_SUCCESS &&
           vkBindImageMemory(dev_, *img, *mem, 0) == VK_SUCCESS;
  }

  //! One-shot command buffer on the app's queue (the main thread renders on it too).
  template <typename F> bool GpuRun(F &&record) {
    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pci.queueFamilyIndex = family_;
      if (vkCreateCommandPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS) {
        return false;
      }
    }
    VkCommandBufferAllocateInfo cai = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev_, &cai, &cmd) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const bool ok = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
                    vkQueueWaitIdle(queue_) == VK_SUCCESS;
    vkFreeCommandBuffers(dev_, pool_, 1, &cmd);
    return ok;
  }

  //! A host-visible buffer (staging for the one upload and the row readbacks).
  bool GpuBuffer(VkDeviceSize size) {
    if (buf_ != VK_NULL_HANDLE && buf_size_ >= size) {
      return true;
    }
    if (buf_ != VK_NULL_HANDLE) {
      vkUnmapMemory(dev_, buf_mem_);
      vkDestroyBuffer(dev_, buf_, nullptr);
      vkFreeMemory(dev_, buf_mem_, nullptr);
      buf_ = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(dev_, &bci, nullptr, &buf_) != VK_SUCCESS) {
      buf_ = VK_NULL_HANDLE;
      return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, buf_, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd_, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    while (type < mp.memoryTypeCount &&
           !((req.memoryTypeBits & (1u << type)) != 0 &&
             (mp.memoryTypes[type].propertyFlags & want) == want)) {
      type++;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (type >= mp.memoryTypeCount ||
        vkAllocateMemory(dev_, &ai, nullptr, &buf_mem_) != VK_SUCCESS ||
        vkBindBufferMemory(dev_, buf_, buf_mem_, 0) != VK_SUCCESS ||
        vkMapMemory(dev_, buf_mem_, 0, VK_WHOLE_SIZE, 0, &buf_map_) != VK_SUCCESS) {
      return false;
    }
    buf_size_ = size;
    return true;
  }

  static void Barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from,
                      VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }

  //! The input: a global RGBA IOSurface behind an app VkImage, the frame
  //! uploaded into it once through Vulkan (the app's "decoded image").
  bool GpuMakeInput() {
    if (in_surf_ != nullptr) {
      return true;
    }
    int32_t w = (int32_t)kW, h = (int32_t)kH, bpe = 4, fourcc = 'RGBA';
    CFNumberRef nw = CFNumberCreate(nullptr, kCFNumberSInt32Type, &w);
    CFNumberRef nh = CFNumberCreate(nullptr, kCFNumberSInt32Type, &h);
    CFNumberRef nb = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bpe);
    CFNumberRef nf = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fourcc);
    const void *keys[5] = {CFSTR("IOSurfaceWidth"), CFSTR("IOSurfaceHeight"),
                           CFSTR("IOSurfaceBytesPerElement"),
                           CFSTR("IOSurfacePixelFormat"),
                           CFSTR("IOSurfaceIsGlobal")};
    const void *vals[5] = {nw, nh, nb, nf, kCFBooleanTrue};
    CFDictionaryRef props =
        CFDictionaryCreate(nullptr, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    in_surf_ = IOSurfaceCreate(props);
    CFRelease(props);
    CFRelease(nw);
    CFRelease(nh);
    CFRelease(nb);
    CFRelease(nf);
    if (in_surf_ == nullptr || !GpuImport(in_surf_, kW, kH, &in_img_, &in_mem_) ||
        !GpuBuffer((VkDeviceSize)kW * kH * 4u)) {
      return false;
    }
    memcpy(buf_map_, pixels_.data(), pixels_.size());
    return GpuRun([&](VkCommandBuffer cmd) {
      Barrier(cmd, in_img_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
      VkBufferImageCopy r = {};
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageExtent = {kW, kH, 1};
      vkCmdCopyBufferToImage(cmd, buf_, in_img_, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
      Barrier(cmd, in_img_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    });
  }

  //! A new export IOSurface (the caller's reference): import it.
  void GpuTakeExport(IOSurfaceRef s, uint32_t w, uint32_t h) {
    GpuReleaseExport();
    out_surf_ = s; // ours now (CFRelease when replaced)
    out_w_ = w;
    out_h_ = h;
    if (!GpuImport(s, w, h, &out_img_, &out_mem_)) {
      printf("[lift-probe] GPU arm: the export IOSurface did not import\n");
      GpuReleaseExport();
      return;
    }
    export_imports_++;
    printf("[lift-probe] GPU arm: export IOSurface %u (%ux%u) imported into the "
           "app's VkImage\n",
           (unsigned)IOSurfaceGetID(s), w, h);
  }

  //! Row @p y of the export through Vulkan; checked against the IOSurface's
  //! CPU view now and then (the bytes must match exactly).
  bool GpuReadRow(uint32_t y, std::vector<uint8_t> &out) {
    const VkDeviceSize bytes = (VkDeviceSize)out_w_ * 4u;
    if (!GpuBuffer(std::max<VkDeviceSize>(bytes, (VkDeviceSize)kW * kH * 4u))) {
      return false;
    }
    const bool ok = GpuRun([&](VkCommandBuffer cmd) {
      Barrier(cmd, out_img_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
      VkBufferImageCopy r = {};
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageOffset = {0, (int32_t)y, 0};
      r.imageExtent = {out_w_, 1, 1};
      vkCmdCopyImageToBuffer(cmd, out_img_, VK_IMAGE_LAYOUT_GENERAL, buf_, 1, &r);
    });
    if (!ok) {
      return false;
    }
    out.assign((const uint8_t *)buf_map_, (const uint8_t *)buf_map_ + bytes);
    if (acquired_ <= 3 || acquired_ % 30 == 0) {
      IOSurfaceLock(out_surf_, kIOSurfaceLockReadOnly, nullptr);
      const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(out_surf_) +
                            (size_t)y * IOSurfaceGetBytesPerRow(out_surf_);
      export_checks_++;
      export_match_ += memcmp(base, out.data(), (size_t)bytes) == 0 ? 1u : 0u;
      IOSurfaceUnlock(out_surf_, kIOSurfaceLockReadOnly, nullptr);
    }
    return true;
  }

  void GpuReleaseExport() {
    if (out_img_ != VK_NULL_HANDLE) {
      vkDestroyImage(dev_, out_img_, nullptr);
      out_img_ = VK_NULL_HANDLE;
    }
    if (out_mem_ != VK_NULL_HANDLE) {
      vkFreeMemory(dev_, out_mem_, nullptr);
      out_mem_ = VK_NULL_HANDLE;
    }
    if (out_surf_ != nullptr) {
      CFRelease(out_surf_);
      out_surf_ = nullptr;
    }
  }

  void GpuRelease() {
    if (dev_ == VK_NULL_HANDLE) {
      return;
    }
    vkDeviceWaitIdle(dev_);
    GpuReleaseExport();
    if (in_img_ != VK_NULL_HANDLE) {
      vkDestroyImage(dev_, in_img_, nullptr);
      in_img_ = VK_NULL_HANDLE;
    }
    if (in_mem_ != VK_NULL_HANDLE) {
      vkFreeMemory(dev_, in_mem_, nullptr);
      in_mem_ = VK_NULL_HANDLE;
    }
    if (in_surf_ != nullptr) {
      CFRelease(in_surf_);
      in_surf_ = nullptr;
    }
    if (buf_ != VK_NULL_HANDLE) {
      vkUnmapMemory(dev_, buf_mem_);
      vkDestroyBuffer(dev_, buf_, nullptr);
      vkFreeMemory(dev_, buf_mem_, nullptr);
      buf_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(dev_, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
    }
  }

  VkPhysicalDevice pd_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  PFN_vkExportMetalObjectsEXT export_ = nullptr;
  PFN_vkGetMemoryMetalHandlePropertiesEXT mem_props_ = nullptr;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkBuffer buf_ = VK_NULL_HANDLE;
  VkDeviceMemory buf_mem_ = VK_NULL_HANDLE;
  void *buf_map_ = nullptr;
  VkDeviceSize buf_size_ = 0;
  IOSurfaceRef in_surf_ = nullptr;
  VkImage in_img_ = VK_NULL_HANDLE;
  VkDeviceMemory in_mem_ = VK_NULL_HANDLE;
  IOSurfaceRef out_surf_ = nullptr;
  VkImage out_img_ = VK_NULL_HANDLE;
  VkDeviceMemory out_mem_ = VK_NULL_HANDLE;
  uint32_t out_w_ = 0, out_h_ = 0;
  std::vector<uint8_t> gpu_frame_;
#endif
  bool gpu_live_ = false; //!< the stream runs the GPU arm
  double submit_ns_ = 0.0, lat_ns_ = 0.0;
  uint32_t lat_samples_ = 0;
  uint64_t last_out_bytes_ = 0;
  uint32_t export_imports_ = 0, export_checks_ = 0, export_match_ = 0;

  PFN_xrEnumerateLiftTasksDXR enumerate_ = nullptr;
  PFN_xrCreateLiftStreamDXR create_ = nullptr;
  PFN_xrDestroyLiftStreamDXR destroy_ = nullptr;
  PFN_xrSubmitLiftFrameDXR submit_ = nullptr;
  PFN_xrAcquireLiftResultDXR acquire_ = nullptr;
  PFN_xrGetLiftStreamStatsDXR stats_ = nullptr;

  XrLiftStreamDXR stream_ = XR_NULL_HANDLE;
  std::vector<uint8_t> pixels_;
  uint32_t every_ = 10, budget_ = 0;
  uint64_t frame_ = 0, streamed_ = 0;
  uint32_t submitted_ = 0, submit_failed_ = 0, acquired_ = 0, not_ready_ = 0,
           acquire_failed_ = 0, skipped_ = 0;
  uint32_t transitions_ = 0, shift_samples_ = 0;
  int last_shift_ = 0;
  XrResult last_enum_result_ = XR_SUCCESS;
  char last_sig_[256] =
      "?"; //!< "?" = nothing printed yet: the first enumeration always logs
  bool done_ = false;
};

} // namespace dxr_test
