// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera consumer probe for Android: the Android stand-in
 *         for `displayxr-cli camera probe`
 * (docs/roadmap/stereo-camera-android.md §12.3).
 *
 * A CAMERA_CONSUMER-only XrInstance (no session): enumerate, read the
 * calibration, create + start one NV12 shared-memory stream, map the sealed
 * memfd read-only, wait on the per-stream eventfd, acquire for N seconds, then
 * report:
 *   - delivered rate and frameIndex gaps,
 *   - captureTime -> now latency (XR_KHR_convert_timespec_time,
 * CLOCK_MONOTONIC),
 *   - ROW ALIGNMENT of the last frame: 2-D block matching (dx AND dy, sub-pixel
 *     dy) of textured 32x32 blocks of the left eye against the right eye: the
 *     median / p90 |dy| over the whole eye and in the outer 15 % columns (a
 *     rectified pair must bring them to ~0), and the sign of the horizontal
 *     disparity (positive = correct eye order for a scene in front of the
 * camera),
 *   - the service's stream stats.
 * The last frame is written to files/cam_last.nv12 for eyeballing.
 *
 * Spec v8 (the `v8` extra, see MainActivity): lists every camera's capture
 * modes and control capabilities, runs the stream in a chosen mode (with the
 * calibration of that mode), sets focus / metering / exposure controls and
 * logs the per-frame capture state (focus distance + state, exposure, ISO,
 * per-eye skew), and takes one still halfway through: ready event, read-only
 * section, its capture state, its calibration, the row alignment of the still
 * itself, written to files/still_last.nv12. A runtime older than v8 lacks the
 * functions: the probe says so and runs as before.
 *
 * Vendor-neutral: it knows nothing about which plug-in produced the frames.
 */

#include <jni.h>
#include <android/log.h>

#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_stereo_camera.h>

#define TAG "dxr_camera_probe"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

struct fns
{
	PFN_xrEnumerateStereoCamerasDXR enumerate{};
	PFN_xrGetStereoCameraCalibrationDXR calib{};
	PFN_xrCreateStereoCameraStreamDXR create{};
	PFN_xrDestroyStereoCameraStreamDXR destroy{};
	PFN_xrStartStereoCameraStreamDXR start{};
	PFN_xrStopStereoCameraStreamDXR stop{};
	PFN_xrGetStereoCameraStreamInfoDXR info{};
	PFN_xrAcquireStereoCameraFrameDXR acquire{};
	PFN_xrGetStereoCameraStreamStatsDXR stats{};
	PFN_xrConvertTimespecTimeToTimeKHR ts2xr{};
        // Spec v8 (optional: a v7 runtime has none of them).
        PFN_xrEnumerateStereoCameraModesDXR modes{};
        PFN_xrSetStereoCameraControlsDXR set_controls{};
        PFN_xrReleaseStereoCameraControlsDXR release_controls{};
        PFN_xrRequestStereoCameraStillDXR request_still{};
        PFN_xrAcquireStereoCameraStillDXR acquire_still{};
        PFN_xrReleaseStereoCameraStillDXR release_still{};
};

//! The `v8` extra: "key=value;key=value".
std::string v8_opt(const std::string &v8, const char *key) {
        const std::string k = std::string(key) + "=";
        size_t at = 0;
        while (at < v8.size()) {
          size_t end = v8.find(';', at);
          if (end == std::string::npos) {
            end = v8.size();
          }
          if (v8.compare(at, k.size(), k) == 0) {
            return v8.substr(at + k.size(), end - at - k.size());
          }
          at = end + 1;
        }
        return "";
}

const char *focus_state_str(XrStereoCameraFocusStateDXR s) {
        switch (s) {
        case XR_STEREO_CAMERA_FOCUS_STATE_SEARCHING_DXR:
          return "SEARCHING";
        case XR_STEREO_CAMERA_FOCUS_STATE_LOCKED_DXR:
          return "LOCKED";
        case XR_STEREO_CAMERA_FOCUS_STATE_FIXED_DXR:
          return "FIXED";
        case XR_STEREO_CAMERA_FOCUS_STATE_FAILED_DXR:
          return "FAILED";
        default:
          return "UNKNOWN";
        }
}

template <typename T>
bool
get_fn(XrInstance inst, const char *name, T *out)
{
	return XR_SUCCEEDED(xrGetInstanceProcAddr(inst, name, reinterpret_cast<PFN_xrVoidFunction *>(out))) &&
	       *out != nullptr;
}

/*
 * Row alignment: for textured 32x32 blocks of the left eye, search the right
 * eye over dx in [-dmax, dmin] (x_R = x_L - d, d = disparity) and dy in
 * [-ry, ry] by SAD; keep the best when it is clearly better than the median
 * cost (a textured, unambiguous match); refine dy with a parabola.
 */
struct match
{
	float x, dy, d;
};

std::vector<match>
row_alignment(const uint8_t *y, uint32_t pitch, uint32_t eye_w, uint32_t eye_h)
{
	const int B = 32, step = 40, ry = 6, dneg = 8, dpos = 120;
	std::vector<match> out;
	const uint8_t *L = y, *R = y + eye_w;
	for (int by = ry + 8; by + B + ry + 8 < (int)eye_h; by += step) {
		for (int bx = dpos + 8; bx + B + dneg + 8 < (int)eye_w; bx += step) {
			// texture: variance of the left block
			double s = 0, s2 = 0;
			for (int j = 0; j < B; j++) {
				for (int i = 0; i < B; i++) {
					double v = L[(size_t)(by + j) * pitch + bx + i];
					s += v;
					s2 += v * v;
				}
			}
			const double var = s2 / (B * B) - (s / (B * B)) * (s / (B * B));
			if (var < 150.0) {
				continue;
			}
			double best = 1e30, second = 1e30;
			int bd = 0, bdy = 0;
			std::vector<double> costs;
			double cost_dy[2 * 6 + 1];
			for (int d = -dneg; d <= dpos; d++) {
				for (int dy = -ry; dy <= ry; dy++) {
					double c = 0;
					for (int j = 0; j < B; j += 2) {
						const uint8_t *a = L + (size_t)(by + j) * pitch + bx;
						const uint8_t *b = R + (size_t)(by + j + dy) * pitch + bx - d;
						for (int i = 0; i < B; i += 2) {
							c += std::abs((int)a[i] - (int)b[i]);
						}
					}
					costs.push_back(c);
					if (c < best) {
						second = best;
						best = c;
						bd = d;
						bdy = dy;
					} else if (c < second) {
						second = c;
					}
				}
			}
			std::nth_element(costs.begin(), costs.begin() + costs.size() / 2, costs.end());
			const double med = costs[costs.size() / 2];
			if (!(best < 0.5 * med) || bdy == -ry || bdy == ry || bd == -dneg || bd == dpos) {
				continue;
			}
			// full-resolution cost along dy at the best d, for the sub-pixel fit
			for (int dy = -ry; dy <= ry; dy++) {
				double c = 0;
				for (int j = 0; j < B; j++) {
					const uint8_t *a = L + (size_t)(by + j) * pitch + bx;
					const uint8_t *b = R + (size_t)(by + j + dy) * pitch + bx - bd;
					for (int i = 0; i < B; i++) {
						c += std::abs((int)a[i] - (int)b[i]);
					}
				}
				cost_dy[dy + ry] = c;
			}
			const double cm = cost_dy[bdy + ry - 1], c0 = cost_dy[bdy + ry], cp = cost_dy[bdy + ry + 1];
			const double den = cm - 2 * c0 + cp;
			const double sub = den > 1e-9 ? 0.5 * (cm - cp) / den : 0.0;
			out.push_back({(float)(bx + B / 2), (float)(bdy + std::max(-0.5, std::min(0.5, sub))), (float)bd});
		}
	}
	return out;
}

float
pct(std::vector<float> v, double p)
{
	if (v.empty()) {
		return NAN;
	}
	std::sort(v.begin(), v.end());
	return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
}

std::string
fmt(const char *f, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	return b;
}

/*!
 * Spec v8 §5.9: the online vertical-alignment correction a RAW consumer applies
 * to its own rectification (D(y) = offset + slope * y, right minus left,
 * normalised rectified coordinates). A v8 runtime without it leaves the struct
 * as passed: flags 0 = not valid.
 */
std::string refinement_line(PFN_xrGetStereoCameraCalibrationDXR calib,
                            XrInstance inst, uint64_t camera_id) {
        XrStereoCameraVerticalRefinementDXR vr = {
            XR_TYPE_STEREO_CAMERA_VERTICAL_REFINEMENT_DXR};
        XrStereoCameraCalibrationDXR cal = {
            XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR};
        cal.next = &vr;
        cal.output = XR_STEREO_CAMERA_OUTPUT_RAW_DXR;
        XrResult r = calib(inst, camera_id, &cal);
        if (XR_FAILED(r)) {
                return fmt("vertical refinement: query -> %d", (int)r);
        }
        if ((vr.flags & XR_STEREO_CAMERA_VERTICAL_REFINEMENT_VALID_BIT_DXR) ==
            0) {
                return "vertical refinement: not valid yet";
        }
        return fmt("vertical refinement: D = %+.6f %+.6f*y (= %+.2f px at "
                   "%dx%d f %.1f), residual %+.2f px, "
                   "%u matches, %u windows, %u updates; R1 q(%.4f %.4f %.4f "
                   "%.4f) R2 q(%.4f %.4f %.4f %.4f)",
                   vr.verticalOffset, vr.verticalSlope,
                   vr.verticalOffset * vr.measuredFocalLength,
                   vr.measuredEyeExtent.width, vr.measuredEyeExtent.height,
                   vr.measuredFocalLength, vr.residualPx, vr.matchCount,
                   vr.windowCount, vr.updateCount, vr.rectifyingRotation[0].x,
                   vr.rectifyingRotation[0].y, vr.rectifyingRotation[0].z,
                   vr.rectifyingRotation[0].w, vr.rectifyingRotation[1].x,
                   vr.rectifyingRotation[1].y, vr.rectifyingRotation[1].z,
                   vr.rectifyingRotation[1].w);
}

/*!
 * Spec v8: acquire the ready still @p id — its read-only section, capture state
 * and calibration — measure its rows, write its pixels to @p path, release it.
 */
std::string take_still(const fns &f, XrStereoCameraStreamDXR stream,
                       uint64_t id, int64_t latency_ns,
                       const std::string &path) {
        XrStereoCameraCaptureResultDXR cr = {
            XR_TYPE_STEREO_CAMERA_CAPTURE_RESULT_DXR};
        XrStereoCameraCalibrationDXR cal = {
            XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR};
        cal.next = &cr;
        XrStereoCameraStillDXR st = {XR_TYPE_STEREO_CAMERA_STILL_DXR};
        st.next = &cal;
        XrResult r = f.acquire_still(stream, id, &st);
        if (XR_FAILED(r)) {
                f.release_still(stream, id);
                return fmt("still %llu: acquire -> %d", (unsigned long long)id,
                           (int)r);
        }
        const int fd = (int)st.sectionHandle;
        void *map = fd >= 0 ? mmap(nullptr, st.sectionSize, PROT_READ,
                                   MAP_SHARED, fd, 0)
                            : MAP_FAILED;
        std::string align = "not mapped";
        if (map != MAP_FAILED) {
                const uint8_t *y = (const uint8_t *)map + st.planeOffset[0];
                if (st.format == XR_STEREO_CAMERA_FORMAT_NV12_DXR ||
                    st.format == XR_STEREO_CAMERA_FORMAT_GRAY8_DXR) {
                        std::vector<match> m = row_alignment(
                            y, st.rowPitch[0], (uint32_t)st.extent.width / 2,
                            (uint32_t)st.extent.height);
                        std::vector<float> ady, d;
                        for (const auto &k : m) {
                                ady.push_back(std::fabs(k.dy));
                                d.push_back(k.d);
                        }
                        align = fmt("%zu blocks, |dy| median %.2f p90 %.2f px, "
                                    "disparity median %.1f px",
                                    m.size(), pct(ady, 0.5), pct(ady, 0.9),
                                    pct(d, 0.5));
                }
                FILE *fp = fopen(path.c_str(), "wb");
                if (fp != nullptr) {
                        fwrite(map, 1, (size_t)st.sectionSize, fp);
                        fclose(fp);
                }
                munmap(map, st.sectionSize);
        }
        if (fd >= 0) {
                close(fd);
        }
        f.release_still(stream, id);
        const std::string s =
            fmt("still %llu in %.0f ms: %dx%d %s fmt %d, %llu B; capture focus "
                "%.3f D %s ISO %u (valid 0x%llx); calibration "
                "%dx%d/eye f %.1f c %.1f,%.1f baseline %.2f mm; rows: %s; "
                "%s; written to %s",
                (unsigned long long)st.requestId, latency_ns / 1e6,
                st.extent.width, st.extent.height,
                st.output == XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR ? "RECTIFIED"
                                                                   : "RAW",
                (int)st.format, (unsigned long long)st.sectionSize,
                cr.focusDistanceDiopters, focus_state_str(cr.focusState),
                cr.sensitivityIso, (unsigned long long)cr.validFields,
                cal.eye[0].imageExtent.width, cal.eye[0].imageExtent.height,
                cal.eye[0].fx, cal.eye[0].cx, cal.eye[0].cy, cal.baselineMm,
                align.c_str(),
                (st.sourceFlags & XR_STEREO_CAMERA_STILL_SOURCE_LOSSY_BIT_DXR)
                    ? "LOSSY source"
                    : "lossless source",
                path.c_str());
        LOGW("%s", s.c_str());
        return s;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_displayxr_camera_1probe_1android_MainActivity_nativeRun(
    JNIEnv *env, jclass, jobject activity, jstring jcam, jint secs,
    jstring joutput, jstring jout, jstring jv8) {
        std::string result;
	auto done = [&](const std::string &s) {
		LOGW("%s", s.c_str());
		return env->NewStringUTF(s.c_str());
	};
	const char *c = env->GetStringUTFChars(jcam, nullptr);
	std::string cam_sel = c;
	env->ReleaseStringUTFChars(jcam, c);
	c = env->GetStringUTFChars(joutput, nullptr);
	const bool want_raw = std::string(c) == "raw";
	env->ReleaseStringUTFChars(joutput, c);
	c = env->GetStringUTFChars(jout, nullptr);
	std::string out_path = c;
	env->ReleaseStringUTFChars(jout, c);
        c = env->GetStringUTFChars(jv8, nullptr);
        const std::string v8 = c;
        env->ReleaseStringUTFChars(jv8, c);
        const std::string still_path =
            out_path.substr(0, out_path.rfind('/') + 1) + "still_last.nv12";

        JavaVM *vm = nullptr;
	env->GetJavaVM(&vm);
	PFN_xrInitializeLoaderKHR init_loader = nullptr;
	if (!get_fn(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &init_loader)) {
		return done("FAIL no xrInitializeLoaderKHR");
	}
	XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	li.applicationVM = vm;
	li.applicationContext = activity;
	init_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li));

	// CAMERA_CONSUMER only: a camera-only connection that can never create a session.
	XrStereoCameraClientInfoDXR client = {XR_TYPE_STEREO_CAMERA_CLIENT_INFO_DXR};
	client.flags = XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR;
	XrInstanceCreateInfoAndroidKHR aci = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	aci.next = &client;
	aci.applicationVM = vm;
	aci.applicationActivity = activity;
	const char *exts[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_DXR_STEREO_CAMERA_EXTENSION_NAME,
	                      XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	ci.next = &aci;
	snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "camera_probe_android");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ci.enabledExtensionCount = 3;
	ci.enabledExtensionNames = exts;
	XrInstance inst = XR_NULL_HANDLE;
	const int64_t t_inst0 = []() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }();
	XrResult r = xrCreateInstance(&ci, &inst);
	if (XR_FAILED(r)) {
		return done(fmt("FAIL xrCreateInstance %d", (int)r));
	}
	auto mono = []() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; };
	LOGW("instance up in %.0f ms (CONSUMER_ONLY)", (mono() - t_inst0) / 1e6);

	fns f;
	if (!get_fn(inst, "xrEnumerateStereoCamerasDXR", &f.enumerate) ||
	    !get_fn(inst, "xrGetStereoCameraCalibrationDXR", &f.calib) ||
	    !get_fn(inst, "xrCreateStereoCameraStreamDXR", &f.create) ||
	    !get_fn(inst, "xrDestroyStereoCameraStreamDXR", &f.destroy) ||
	    !get_fn(inst, "xrStartStereoCameraStreamDXR", &f.start) ||
	    !get_fn(inst, "xrStopStereoCameraStreamDXR", &f.stop) ||
	    !get_fn(inst, "xrGetStereoCameraStreamInfoDXR", &f.info) ||
	    !get_fn(inst, "xrAcquireStereoCameraFrameDXR", &f.acquire) ||
	    !get_fn(inst, "xrGetStereoCameraStreamStatsDXR", &f.stats) ||
	    !get_fn(inst, "xrConvertTimespecTimeToTimeKHR", &f.ts2xr)) {
		xrDestroyInstance(inst);
		return done("FAIL extension functions missing");
	}
        // Spec v8: optional.
        const bool have_v8 =
            get_fn(inst, "xrEnumerateStereoCameraModesDXR", &f.modes) &&
            get_fn(inst, "xrSetStereoCameraControlsDXR", &f.set_controls) &&
            get_fn(inst, "xrReleaseStereoCameraControlsDXR",
                   &f.release_controls) &&
            get_fn(inst, "xrRequestStereoCameraStillDXR", &f.request_still) &&
            get_fn(inst, "xrAcquireStereoCameraStillDXR", &f.acquire_still) &&
            get_fn(inst, "xrReleaseStereoCameraStillDXR", &f.release_still);
        LOGW("spec v8 (modes / controls / stills): %s",
             have_v8 ? "available" : "NOT available (runtime older than v8)");
        XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	xrGetSystem(inst, &sgi, &sys);

	uint32_t n = 0;
	r = f.enumerate(inst, sys, 0, &n, nullptr);
	std::vector<XrStereoCameraPropertiesDXR> cams(n, {XR_TYPE_STEREO_CAMERA_PROPERTIES_DXR});
        // Spec v8: each camera's control capabilities, chained
        // (zero-initialised: an older runtime leaves them untouched = no
        // controls).
        std::vector<XrStereoCameraControlCapabilitiesDXR> caps(
            n, {XR_TYPE_STEREO_CAMERA_CONTROL_CAPABILITIES_DXR});
        for (uint32_t i = 0; i < n; i++) {
                // 0 would read "fixed at infinity": pre-set the unknown value.
                caps[i].defaultFocusDistanceDiopters =
                    XR_STEREO_CAMERA_FOCUS_DISTANCE_UNKNOWN_DXR;
                cams[i].next = &caps[i];
        }
        if (n > 0) {
		r = f.enumerate(inst, sys, n, &n, cams.data());
	}
	LOGW("enumerate -> %d, %u camera(s)", (int)r, n);
	int pick = -1;
	for (uint32_t i = 0; i < n; i++) {
		const auto &p = cams[i];
		LOGW("  [%u] id=%llu \"%s\" flags=0x%llx state=%d eye=%dx%d rate=%.2f baseline=%.2f mm hfov=%.1f fmts=0x%llx "
		     "transports=0x%llx",
		     i, (unsigned long long)p.cameraId, p.displayName, (unsigned long long)p.flags, (int)p.state,
		     p.eyeExtent.width, p.eyeExtent.height, p.maxFrameRate, p.baselineMm, p.horizontalFovDeg,
		     (unsigned long long)p.supportedFormats, (unsigned long long)p.supportedTransports);
                if (have_v8) {
                        uint32_t mc = 0;
                        f.modes(inst, p.cameraId, 0, &mc, nullptr);
                        std::vector<XrStereoCameraModeDXR> ms(
                            mc, {XR_TYPE_STEREO_CAMERA_MODE_DXR});
                        if (mc > 0 && XR_SUCCEEDED(f.modes(inst, p.cameraId, mc,
                                                           &mc, ms.data()))) {
                                for (const auto &m : ms) {
                                        LOGW(
                                            "      mode %dx%d per eye %.1f "
                                            "Hz%s%s%s",
                                            m.eyeExtent.width,
                                            m.eyeExtent.height, m.maxFrameRate,
                                            (m.flags &
                                             XR_STEREO_CAMERA_MODE_DEFAULT_BIT_DXR)
                                                ? " DEFAULT"
                                                : "",
                                            (m.flags &
                                             XR_STEREO_CAMERA_MODE_STREAM_BIT_DXR)
                                                ? " STREAM"
                                                : "",
                                            (m.flags &
                                             XR_STEREO_CAMERA_MODE_STILL_BIT_DXR)
                                                ? " STILL"
                                                : "");
                                }
                        }
                        const auto &k = caps[i];
                        LOGW("      controls 0x%llx focus modes 0x%llx nearest "
                             "%.2f D (calibration %d) EV %+.2f..%+.2f step "
                             "%.3f, default focus %.3f D (<0 = auto/unknown)",
                             (unsigned long long)k.supportedControls,
                             (unsigned long long)k.supportedFocusModes,
                             k.minFocusDistanceDiopters,
                             (int)k.focusDistanceCalibration,
                             k.exposureCompensationMinEv,
                             k.exposureCompensationMaxEv,
                             k.exposureCompensationStepEv,
                             k.defaultFocusDistanceDiopters);
                }
                const bool front = (p.flags & XR_STEREO_CAMERA_USER_FACING_BIT_DXR) != 0;
		if (pick < 0 && ((cam_sel == "front" && front) || (cam_sel == "rear" && !front) ||
		                 (isdigit((unsigned char)cam_sel[0]) && (uint32_t)atoi(cam_sel.c_str()) == i))) {
			pick = (int)i;
		}
	}
	if (pick < 0) {
		xrDestroyInstance(inst);
		return done(fmt("FAIL no %s camera (%u enumerated)", cam_sel.c_str(), n));
	}
	const XrStereoCameraPropertiesDXR &cam = cams[pick];
	const XrStereoCameraOutputDXR want_out = want_raw ? XR_STEREO_CAMERA_OUTPUT_RAW_DXR : XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR;
        // Spec v8: the stream's mode — `mode=WxH`, or `mode=max` for the
        // largest STREAM mode.
        XrStereoCameraStreamModeInfoDXR mode_info = {
            XR_TYPE_STEREO_CAMERA_STREAM_MODE_INFO_DXR};
        std::vector<XrStereoCameraModeDXR> cam_modes;
        if (have_v8) {
                uint32_t mc = 0;
                f.modes(inst, cam.cameraId, 0, &mc, nullptr);
                cam_modes.assign(mc, {XR_TYPE_STEREO_CAMERA_MODE_DXR});
                if (mc == 0 || XR_FAILED(f.modes(inst, cam.cameraId, mc, &mc,
                                                 cam_modes.data()))) {
                        cam_modes.clear();
                }
        }
        const std::string mode_opt = v8_opt(v8, "mode");
        if (!cam_modes.empty() && mode_opt == "max") {
                int64_t best = 0;
                for (const auto &m : cam_modes) {
                        const int64_t a =
                            (int64_t)m.eyeExtent.width * m.eyeExtent.height;
                        if ((m.flags & XR_STEREO_CAMERA_MODE_STREAM_BIT_DXR) !=
                                0 &&
                            a > best) {
                                best = a;
                                mode_info.eyeExtent = m.eyeExtent;
                        }
                }
        } else if (!cam_modes.empty() && !mode_opt.empty()) {
                sscanf(mode_opt.c_str(), "%dx%d", &mode_info.eyeExtent.width,
                       &mode_info.eyeExtent.height);
        }
        const bool use_mode =
            mode_info.eyeExtent.width > 0 && mode_info.eyeExtent.height > 0;
        LOGW("stream mode: %s",
             use_mode ? fmt("%dx%d per eye", mode_info.eyeExtent.width,
                            mode_info.eyeExtent.height)
                            .c_str()
                      : "the default");

        XrStereoCameraCalibrationDXR cal = {XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR};
        cal.next = use_mode ? &mode_info
                            : nullptr; // v8: the numbers of THIS mode's frames
        cal.output = want_out;
	r = f.calib(inst, cam.cameraId, &cal);
	if (XR_SUCCEEDED(r)) {
		LOGW("calibration (%s): L f=%.1f/%.1f c=%.1f/%.1f  R f=%.1f/%.1f c=%.1f/%.1f  baseline %.2f mm  "
		     "rightFromLeft p=(%.2f %.2f %.2f) mm",
		     want_raw ? "RAW" : "RECTIFIED", cal.eye[0].fx, cal.eye[0].fy, cal.eye[0].cx, cal.eye[0].cy,
		     cal.eye[1].fx, cal.eye[1].fy, cal.eye[1].cx, cal.eye[1].cy, cal.baselineMm,
		     cal.rightFromLeft.position.x * 1000.f, cal.rightFromLeft.position.y * 1000.f,
		     cal.rightFromLeft.position.z * 1000.f);
	} else {
		LOGW("calibration (%s) -> %d", want_raw ? "RAW" : "RECTIFIED", (int)r);
	}
        LOGW("%s", refinement_line(f.calib, inst, cam.cameraId).c_str());

        XrStereoCameraStreamCreateInfoDXR sci = {XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR};
        sci.next =
            use_mode ? &mode_info : nullptr; // v8: absent = the default mode
        sci.cameraId = cam.cameraId;
	sci.output = want_out;
	sci.format = XR_STEREO_CAMERA_FORMAT_NV12_DXR;
	sci.transport = XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR;
	sci.maxFrameRate = 0.0f;
	XrStereoCameraStreamDXR stream = XR_NULL_HANDLE;
	r = f.create(inst, &sci, &stream);
	if (XR_FAILED(r)) {
		xrDestroyInstance(inst);
		return done(fmt("FAIL create stream %d", (int)r));
	}
	const int64_t t_start = mono();
	r = f.start(stream);
	if (XR_FAILED(r)) {
		f.destroy(stream);
		xrDestroyInstance(inst);
		return done(fmt("FAIL start stream %d (consent? set debug.xrt.DXR_STEREO_CAMERA_DEV_ALLOW=1 on a dev box)",
		                (int)r));
	}
	XrStereoCameraStreamTransportDXR tr = {XR_TYPE_STEREO_CAMERA_STREAM_TRANSPORT_DXR};
	XrStereoCameraStreamInfoDXR si = {XR_TYPE_STEREO_CAMERA_STREAM_INFO_DXR};
	si.next = &tr;
	r = f.info(stream, &si);
	const int sec_fd = (int)tr.sectionHandle, wake_fd = (int)tr.wakeHandle;
	void *map = XR_SUCCEEDED(r) && sec_fd >= 0 && tr.sectionSize > 0
	                ? mmap(nullptr, tr.sectionSize, PROT_READ, MAP_SHARED, sec_fd, 0)
	                : MAP_FAILED;
	LOGW("stream: %dx%d fmt=%d output=%d transport=%d rate=%.2f; section fd=%d size=%llu slots=%u stride=%llu wake fd=%d "
	     "map=%s",
	     si.extent.width, si.extent.height, (int)si.format, (int)si.output, (int)si.transport, si.maxFrameRate, sec_fd,
	     (unsigned long long)tr.sectionSize, tr.slotCount, (unsigned long long)tr.slotStride, wake_fd,
	     map == MAP_FAILED ? "FAILED" : "ok");
	if (map == MAP_FAILED) {
		f.stop(stream);
		f.destroy(stream);
		xrDestroyInstance(inst);
		return done("FAIL cannot map the ring");
	}

	uint64_t frames = 0, gaps = 0, last_index = 0, not_ready = 0;
	double lat_sum = 0, lat_max = 0;
	int64_t first_ns = 0;
	XrStereoCameraFrameDXR last = {};
	std::vector<uint8_t> last_buf;
	bool ended = false;

        // Spec v8 controls (`focus=auto|once|manual:<D>|locked`, `point=x,y`,
        // `ev=<EV>`), set once the first frame is in; and one still
        // (`still=WxH|max`) halfway.
        XrStereoCameraControlsDXR ctl = {XR_TYPE_STEREO_CAMERA_CONTROLS_DXR};
        {
                const std::string fo = v8_opt(v8, "focus"),
                                  po = v8_opt(v8, "point"),
                                  eo = v8_opt(v8, "ev");
                if (!fo.empty()) {
                        ctl.validControls |=
                            XR_STEREO_CAMERA_CONTROL_FOCUS_MODE_BIT_DXR;
                        ctl.focusMode =
                            fo == "once"
                                ? XR_STEREO_CAMERA_FOCUS_MODE_AUTO_ONCE_DXR
                            : fo.compare(0, 6, "manual") == 0
                                ? XR_STEREO_CAMERA_FOCUS_MODE_MANUAL_DXR
                            : fo == "locked"
                                ? XR_STEREO_CAMERA_FOCUS_MODE_LOCKED_DXR
                                : XR_STEREO_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO_DXR;
                        if (ctl.focusMode ==
                                XR_STEREO_CAMERA_FOCUS_MODE_MANUAL_DXR &&
                            fo.size() > 7) {
                                ctl.validControls |=
                                    XR_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE_BIT_DXR;
                                ctl.focusDistanceDiopters =
                                    (float)atof(fo.c_str() + 7);
                        }
                }
                if (!po.empty() &&
                    sscanf(po.c_str(), "%f,%f", &ctl.meteringPoint.x,
                           &ctl.meteringPoint.y) == 2) {
                        ctl.validControls |=
                            XR_STEREO_CAMERA_CONTROL_METERING_POINT_BIT_DXR;
                }
                if (!eo.empty()) {
                        ctl.validControls |=
                            XR_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION_BIT_DXR;
                        ctl.exposureCompensationEv = (float)atof(eo.c_str());
                }
        }
        bool controls_sent = !have_v8 || ctl.validControls == 0;
        const std::string still_opt = have_v8 ? v8_opt(v8, "still") : "";
        bool still_asked = still_opt.empty();
        uint64_t still_id = 0;
        int64_t still_t0 = 0;
        std::string still_report =
            still_opt.empty() ? "no still asked" : "still: never completed";
        int64_t next_result_log = 0;
        uint32_t searching = 0, locked = 0;

        while (mono() - t_start < (int64_t)secs * 1000000000LL && !ended) {
		struct pollfd pfd = {wake_fd, POLLIN, 0};
		if (wake_fd >= 0 && poll(&pfd, 1, 200) > 0) {
			uint64_t v;
			(void)!read(wake_fd, &v, sizeof(v));
		}
		XrStereoCameraFrameDXR fr = {XR_TYPE_STEREO_CAMERA_FRAME_DXR};
                XrStereoCameraCaptureResultDXR cr = {
                    XR_TYPE_STEREO_CAMERA_CAPTURE_RESULT_DXR}; // v8, zeroed
                fr.next = &cr;
                r = f.acquire(stream, &fr);
		if (r == XR_STEREO_CAMERA_FRAME_NOT_READY_DXR) {
			not_ready++;
		} else if (XR_SUCCEEDED(r)) {
			const int64_t now = mono();
			if (first_ns == 0) {
				first_ns = now;
				LOGW("first frame %.0f ms after start: %dx%d rowPitch %u/%u planeOffset %llu/%llu output=%d exposure=%d",
				     (now - t_start) / 1e6, fr.extent.width, fr.extent.height, fr.rowPitch[0], fr.rowPitch[1],
				     (unsigned long long)fr.planeOffset[0], (unsigned long long)fr.planeOffset[1], (int)fr.output,
				     (int)fr.captureTimeIsExposure);
			}
			if (last_index != 0 && fr.frameIndex > last_index + 1) {
				gaps += fr.frameIndex - last_index - 1;
			}
			last_index = fr.frameIndex;
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			XrTime xnow = 0;
			f.ts2xr(inst, &ts, &xnow);
			const double lat = (double)(xnow - fr.captureTime) / 1e6;
			lat_sum += lat;
			lat_max = std::max(lat_max, lat);
			frames++;
			last = fr;
                        last.next = nullptr;
                        // Spec v8: the frame's capture state, logged ~1 Hz.
                        searching +=
                            cr.focusState ==
                                    XR_STEREO_CAMERA_FOCUS_STATE_SEARCHING_DXR
                                ? 1u
                                : 0u;
                        locked +=
                            cr.focusState ==
                                    XR_STEREO_CAMERA_FOCUS_STATE_LOCKED_DXR
                                ? 1u
                                : 0u;
                        if (cr.validFields != 0 && now >= next_result_log) {
                                next_result_log = now + 1000000000LL;
                                LOGW("frame %llu capture: focus %.3f D %s, "
                                     "exposure %.3f ms, ISO %u, eye skew %+.3f "
                                     "ms (valid 0x%llx)",
                                     (unsigned long long)fr.frameIndex,
                                     cr.focusDistanceDiopters,
                                     focus_state_str(cr.focusState),
                                     cr.exposureTime / 1e6, cr.sensitivityIso,
                                     (cr.eyeCaptureTime[1] -
                                      cr.eyeCaptureTime[0]) /
                                         1e6,
                                     (unsigned long long)cr.validFields);
                        }
                        if (!controls_sent) {
                                controls_sent = true;
                                const XrResult cret =
                                    f.set_controls(stream, &ctl);
                                LOGW("set controls 0x%llx (focus mode %d, %.2f "
                                     "D, point %.2f,%.2f, %+.2f EV) -> %d",
                                     (unsigned long long)ctl.validControls,
                                     (int)ctl.focusMode,
                                     ctl.focusDistanceDiopters,
                                     ctl.meteringPoint.x, ctl.meteringPoint.y,
                                     ctl.exposureCompensationEv, (int)cret);
                        }
                        if (!still_asked &&
                            now - t_start >= (int64_t)secs * 500000000LL) {
                                still_asked = true;
                                XrStereoCameraStillRequestDXR rq = {
                                    XR_TYPE_STEREO_CAMERA_STILL_REQUEST_DXR};
                                if (still_opt != "max") {
                                        sscanf(still_opt.c_str(), "%dx%d",
                                               &rq.eyeExtent.width,
                                               &rq.eyeExtent.height);
                                }
                                rq.output = want_out;
                                rq.format = XR_STEREO_CAMERA_FORMAT_NV12_DXR;
                                still_t0 = now;
                                const XrResult sret =
                                    f.request_still(stream, &rq, &still_id);
                                LOGW("request still (%s) -> %d, id %llu",
                                     still_opt.c_str(), (int)sret,
                                     (unsigned long long)still_id);
                                if (XR_FAILED(sret)) {
                                        still_report =
                                            fmt("still: request refused (%d)",
                                                (int)sret);
                                }
                        }
                        // Copy out the newest frame (the slot is only stable until the next acquire).
			const uint8_t *slot = (const uint8_t *)map + (size_t)fr.slot * tr.slotStride;
			const size_t ysz = (size_t)fr.rowPitch[0] * fr.extent.height;
			const size_t uvsz = (size_t)fr.rowPitch[1] * (fr.extent.height / 2);
			last_buf.resize(ysz + uvsz);
			memcpy(last_buf.data(), slot + fr.planeOffset[0], ysz);
			memcpy(last_buf.data() + ysz, slot + fr.planeOffset[1], uvsz);
		} else if (r == XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR) {
			LOGW("stream ENDED by the runtime");
			ended = true;
		} else {
			LOGW("acquire -> %d", (int)r);
		}
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
                        if (ev.type ==
                            XR_TYPE_EVENT_DATA_STEREO_CAMERA_STILL_READY_DXR) {
                                const auto *sr = reinterpret_cast<
                                    const XrEventDataStereoCameraStillReadyDXR
                                        *>(&ev);
                                still_report =
                                    take_still(f, stream, sr->requestId,
                                               mono() - still_t0, still_path);
                        } else if (
                            ev.type ==
                            XR_TYPE_EVENT_DATA_STEREO_CAMERA_STILL_FAILED_DXR) {
                                const auto *sf = reinterpret_cast<
                                    const XrEventDataStereoCameraStillFailedDXR
                                        *>(&ev);
                                still_report =
                                    fmt("still %llu FAILED (%d)",
                                        (unsigned long long)sf->requestId,
                                        (int)sf->result);
                                LOGW("%s", still_report.c_str());
                        } else {
                                LOGW("event type %d", (int)ev.type);
                        }
                        ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
	}
	const double dur = first_ns ? (mono() - first_ns) / 1e9 : 0.0;
	XrStereoCameraStreamStatsDXR st = {XR_TYPE_STEREO_CAMERA_STREAM_STATS_DXR};
	f.stats(stream, &st);
        // §5.9: measured during the run, RAW-only streams included.
        LOGW("after the run: %s",
             refinement_line(f.calib, inst, cam.cameraId).c_str());

        std::string align = "no frame";
	if (!last_buf.empty()) {
		const uint32_t eye_w = (uint32_t)last.extent.width / 2, eye_h = (uint32_t)last.extent.height;
		std::vector<match> m = row_alignment(last_buf.data(), last.rowPitch[0], eye_w, eye_h);
		std::vector<float> ady, ady_edge, d;
		size_t pos = 0;
		for (const auto &k : m) {
			ady.push_back(std::fabs(k.dy));
			if (k.x < 0.15f * eye_w || k.x > 0.85f * eye_w) {
				ady_edge.push_back(std::fabs(k.dy));
			}
			d.push_back(k.d);
			pos += k.d > 0 ? 1 : 0;
		}
		align = fmt("row alignment over %zu blocks: |dy| median %.2f p90 %.2f px; edge columns (%zu blocks) |dy| median "
		            "%.2f p90 %.2f px; disparity median %.1f px p10 %.1f p90 %.1f, %zu/%zu positive",
		            m.size(), pct(ady, 0.5), pct(ady, 0.9), ady_edge.size(), pct(ady_edge, 0.5), pct(ady_edge, 0.9),
		            pct(d, 0.5), pct(d, 0.1), pct(d, 0.9), pos, m.size());
		FILE *fp = fopen(out_path.c_str(), "wb");
		if (fp != nullptr) {
			fwrite(last_buf.data(), 1, last_buf.size(), fp);
			fclose(fp);
		}
	}
	result = fmt("camera \"%s\" %s: %llu frames in %.1f s = %.2f fps, frameIndex gaps %llu, not-ready %llu, latency "
	             "captureTime->acquire avg %.1f max %.1f ms; service: source %.2f fps delivered %.2f fps, published "
	             "%llu skipped %llu, mean latency %.1f ms; %s",
	             cam.displayName, want_raw ? "RAW" : "RECTIFIED", (unsigned long long)frames, dur,
	             dur > 0 ? frames / dur : 0.0, (unsigned long long)gaps, (unsigned long long)not_ready,
	             frames ? lat_sum / frames : 0.0, lat_max, st.sourceFrameRate, st.deliveredFrameRate,
	             (unsigned long long)st.framesPublished, (unsigned long long)st.framesSkipped, st.meanLatencyNs / 1e6,
	             align.c_str());
	munmap(map, tr.sectionSize);
	if (sec_fd >= 0) {
		close(sec_fd);
	}
	if (wake_fd >= 0) {
		close(wake_fd);
	}
        if (have_v8 && ctl.validControls != 0) {
                f.release_controls(stream); // (stop would release them too)
        }
        result += fmt("; v8: frames SEARCHING %u / LOCKED %u; %s", searching,
                      locked, still_report.c_str());
        const int64_t t_stop = mono();
	f.stop(stream);
	f.destroy(stream);
	LOGW("stop + destroy %.0f ms", (mono() - t_stop) / 1e6);
	xrDestroyInstance(inst);
	return done("OK " + result);
}
