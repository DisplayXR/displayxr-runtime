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
 * Every line starts with "[lift-probe]" so a run can be grepped.
 */

#pragma once

#include <openxr/XR_DXR_lift.h>
#include <openxr/openxr.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dxr_test {

class LiftCpuProbe {
public:
  //! `--lift` on the command line or DISPLAYXR_LIFT_PROBE=1.
  static bool Requested(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--lift") == 0) {
        return true;
      }
    }
    const char *e = getenv("DISPLAYXR_LIFT_PROBE");
    return e != nullptr && e[0] == '1';
  }

  bool enabled = false;

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
    XrLiftTaskStreamCreateInfoDXR task = {
        XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR};
    task.task = XR_LIFT_TASK_VIEWS_MONO_DXR;
    task.inputKind = XR_LIFT_INPUT_KIND_MONO_DXR;
    task.viewCount = 2;
    task.exportKind = XR_LIFT_IMAGE_KIND_CPU_DXR;
    XrLiftStreamCreateInfoDXR ci = {XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR, &task};
    ci.contentHint = XR_LIFT_CONTENT_HINT_VIDEO_DXR;
    ci.inputScale = 1.0f;
    r = create_(session, &ci, &stream_);
    printf("[lift-probe] xrCreateLiftStreamDXR(VIEWS_MONO, 2 views, CPU "
           "in/out): %d\n",
           (int)r);
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
    const XrResult r = submit_(stream_, &si, &id);
    if (XR_SUCCEEDED(r)) {
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
    const uint8_t *px = static_cast<const uint8_t *>(res.outputTexture);
    const uint32_t w = (uint32_t)res.extent.width,
                   h = (uint32_t)res.extent.height;
    const uint32_t views = res.viewCount != 0 ? res.viewCount : 1;
    if (px == nullptr || ex.kind != XR_LIFT_IMAGE_KIND_CPU_DXR || w < views ||
        h == 0) {
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
  char last_sig_[256] = {0};
  bool done_ = false;
};

} // namespace dxr_test
