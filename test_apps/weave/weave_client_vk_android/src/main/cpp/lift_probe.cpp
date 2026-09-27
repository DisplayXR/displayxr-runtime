// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// XR_DXR_lift probe — see lift_probe.h.

#include "lift_probe.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "weave_client_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

enum class Mode
{
	Off,
	Weave,
	Explicit,
	Both,
};

//! Explicit-mode input: a small 2D frame, CPU-painted.
constexpr uint32_t kInW = 960;
constexpr uint32_t kInH = 540;
//! Submit-time ring for caller-side latency (indexed by frameId).
constexpr uint32_t kRing = 256;

struct Probe
{
	Mode mode = Mode::Off;
	bool mode_read = false;
	uint64_t max_frames = 300;

	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	PFN_xrGetLiftPropertiesDXR get_props = nullptr;
	PFN_xrCreateLiftStreamDXR create = nullptr;
	PFN_xrDestroyLiftStreamDXR destroy = nullptr;
	PFN_xrSubmitLiftFrameDXR submit = nullptr;
	PFN_xrAcquireLiftResultDXR acquire = nullptr;
	PFN_xrGetLiftStreamStatsDXR stats = nullptr;

	XrLiftStateDXR last_state = XR_LIFT_STATE_MAX_ENUM_DXR;
	bool streams_tried = false;
	XrLiftStreamDXR weave_stream = XR_NULL_HANDLE;
	XrLiftStreamDXR explicit_stream = XR_NULL_HANDLE;

	AHardwareBuffer *in_ahb = nullptr;
	AHardwareBuffer *out_ahb = nullptr; //!< the runtime's result buffer (our reference)
	uint64_t submitted = 0;
	uint64_t not_taken = 0;
	uint64_t results = 0;
	uint64_t submit_ns[kRing] = {};
	double client_lat_sum_ms = 0.0, client_lat_max_ms = 0.0;
	double svc_lat_sum_ms = 0.0, svc_lat_max_ms = 0.0;
	uint64_t last_result_frame = 0;
	uint64_t result_gaps = 0; //!< frames skipped between consecutive results (= dropped from our view)
	bool summary_done = false;
};

Probe g_probe;

uint64_t
mono_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *
state_name(XrLiftStateDXR s)
{
	switch (s) {
	case XR_LIFT_STATE_READY_DXR: return "READY";
	case XR_LIFT_STATE_ACTIVATING_DXR: return "ACTIVATING";
	default: return "UNAVAILABLE";
	}
}

//! Paint the explicit-mode 2D frame: a warm/cool gradient with vertical bars
//! and a walking block — something a shifted-view fake visibly offsets.
void
paint_input(uint64_t frame)
{
	void *ptr = nullptr;
	if (AHardwareBuffer_lock(g_probe.in_ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr) != 0 ||
	    ptr == nullptr) {
		return;
	}
	AHardwareBuffer_Desc d = {};
	AHardwareBuffer_describe(g_probe.in_ahb, &d);
	uint8_t *base = static_cast<uint8_t *>(ptr);
	const uint32_t walk = (uint32_t)((frame % 120) * kInW / 120);
	for (uint32_t y = 0; y < kInH; ++y) {
		uint8_t *row = base + (size_t)y * d.stride * 4u;
		for (uint32_t x = 0; x < kInW; ++x) {
			uint8_t r = (uint8_t)(40 + 180 * x / kInW), g = (uint8_t)(60 + 120 * y / kInH), b = 160;
			if ((x / 60) % 2 == 0 && y > kInH / 3 && y < 2 * kInH / 3) {
				r = g = b = 235;
			}
			if (x >= walk && x < walk + kInW / 30 && y > kInH / 2 - 30 && y < kInH / 2 + 30) {
				r = 250;
				g = 210;
				b = 30;
			}
			row[4 * x + 0] = r;
			row[4 * x + 1] = g;
			row[4 * x + 2] = b;
			row[4 * x + 3] = 255;
		}
	}
	AHardwareBuffer_unlock(g_probe.in_ahb,
	                       nullptr); // CPU writes complete: the input contract
}

void
log_stats(const char *what, XrLiftStreamDXR stream)
{
	if (stream == XR_NULL_HANDLE || g_probe.stats == nullptr) {
		return;
	}
	XrLiftStreamStatsDXR st = {};
	st.type = XR_TYPE_LIFT_STREAM_STATS_DXR;
	if (g_probe.stats(stream, &st) != XR_SUCCESS) {
		return;
	}
	LOGW(
	    "LIFT_PROBE: %s stream stats: submitted=%llu converted=%llu "
	    "dropped=%llu failed=%llu latency "
	    "last=%.1fms avg=%.1fms min=%.1fms max=%.1fms rate=%.1f/s",
	    what, (unsigned long long)st.framesSubmitted, (unsigned long long)st.framesConverted,
	    (unsigned long long)st.framesDropped, (unsigned long long)st.framesFailed, st.latencyLast / 1e6,
	    st.latencyAverage / 1e6, st.latencyMin / 1e6, st.latencyMax / 1e6, st.conversionRate);
}

XrLiftStreamDXR
make_stream(const char *what)
{
	XrLiftStreamCreateInfoDXR ci = {};
	ci.type = XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR;
	ci.mode = XR_LIFT_MODE_SBS_DXR;
	ci.contentHint = XR_LIFT_CONTENT_HINT_VIDEO_DXR;
	ci.inputScale = 1.0f;
	XrLiftStreamDXR s = XR_NULL_HANDLE;
	XrResult r = g_probe.create(g_probe.session, &ci, &s);
	LOGW("LIFT_PROBE: xrCreateLiftStreamDXR(%s, SBS) -> %d", what, (int)r);
	return r == XR_SUCCESS ? s : XR_NULL_HANDLE;
}

void
explicit_frame(uint64_t frame)
{
	Probe &p = g_probe;
	if (p.explicit_stream == XR_NULL_HANDLE || p.summary_done) {
		return;
	}
	if (p.in_ahb == nullptr) {
		AHardwareBuffer_Desc d = {};
		d.width = kInW;
		d.height = kInH;
		d.layers = 1;
		d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
		if (AHardwareBuffer_allocate(&d, &p.in_ahb) != 0) {
			LOGE("LIFT_PROBE: input AHardwareBuffer allocate failed");
			p.in_ahb = nullptr;
			p.summary_done = true;
			return;
		}
	}

	paint_input(frame);
	XrLiftFrameSubmitInfoDXR si = {};
	si.type = XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR;
	si.inputTexture = p.in_ahb;
	si.inputIsDxgi = XR_FALSE;
	si.extent = {(int32_t)kInW, (int32_t)kInH};
	si.sourceTime = (XrTime)frame;
	uint64_t frame_id = 0;
	const uint64_t t0 = mono_ns();
	XrResult r = p.submit(p.explicit_stream, &si, &frame_id);
	const uint64_t submit_cost = mono_ns() - t0;
	if (r != XR_SUCCESS) {
		LOGE("LIFT_PROBE: xrSubmitLiftFrameDXR -> %d", (int)r);
	} else if (frame_id == 0) {
		p.not_taken++; // module still ACTIVATING
	} else {
		p.submitted++;
		p.submit_ns[frame_id % kRing] = t0;
		if (p.submitted <= 3) {
			LOGW("LIFT_PROBE: submit frameId=%llu cost %.2f ms", (unsigned long long)frame_id,
			     submit_cost / 1e6);
		}
	}

	XrLiftResultDXR res = {};
	res.type = XR_TYPE_LIFT_RESULT_DXR;
	r = p.acquire(p.explicit_stream, &res);
	if (r == XR_SUCCESS) {
		p.results++;
		if (res.outputTexture != nullptr) {
			if (p.out_ahb != nullptr) {
				AHardwareBuffer_release(p.out_ahb);
			}
			p.out_ahb = static_cast<AHardwareBuffer *>(res.outputTexture); // ours to release
			LOGW(
			    "LIFT_PROBE: result buffer (re)exported: %dx%d format=0x%llx "
			    "views=%u fence=%p",
			    res.extent.width, res.extent.height, (unsigned long long)res.format, res.viewCount,
			    res.fence);
		}
		const double svc_ms = (double)res.latency / 1e6;
		const uint64_t sent = p.submit_ns[res.frameId % kRing];
		const double cli_ms = sent != 0 ? (double)(mono_ns() - sent) / 1e6 : 0.0;
		p.svc_lat_sum_ms += svc_ms;
		p.client_lat_sum_ms += cli_ms;
		p.svc_lat_max_ms = svc_ms > p.svc_lat_max_ms ? svc_ms : p.svc_lat_max_ms;
		p.client_lat_max_ms = cli_ms > p.client_lat_max_ms ? cli_ms : p.client_lat_max_ms;
		if (p.last_result_frame != 0 && res.frameId > p.last_result_frame + 1) {
			p.result_gaps += res.frameId - p.last_result_frame - 1;
		}
		p.last_result_frame = res.frameId;
		if (p.results <= 5) {
			LOGW(
			    "LIFT_PROBE: result frameId=%llu sourceTime=%lld %dx%d views=%u "
			    "service=%.1f ms "
			    "submit->acquire=%.1f ms",
			    (unsigned long long)res.frameId, (long long)res.sourceTime, res.extent.width,
			    res.extent.height, res.viewCount, svc_ms, cli_ms);
		}
	} else if (r != XR_LIFT_NOT_READY_DXR) {
		LOGE("LIFT_PROBE: xrAcquireLiftResultDXR -> %d", (int)r);
	}

	if (p.submitted >= p.max_frames && !p.summary_done) {
		p.summary_done = true;
		const double n = p.results > 0 ? (double)p.results : 1.0;
		LOGW(
		    "LIFT_PROBE: SUMMARY explicit: submitted=%llu not_taken=%llu "
		    "results=%llu "
		    "service latency avg=%.1f max=%.1f ms, submit->acquire avg=%.1f "
		    "max=%.1f ms, frames skipped "
		    "between results=%llu",
		    (unsigned long long)p.submitted, (unsigned long long)p.not_taken, (unsigned long long)p.results,
		    p.svc_lat_sum_ms / n, p.svc_lat_max_ms, p.client_lat_sum_ms / n, p.client_lat_max_ms,
		    (unsigned long long)p.result_gaps);
		log_stats("explicit", p.explicit_stream);
	}
}

} // namespace

bool
lift_probe_enabled()
{
	Probe &p = g_probe;
	if (!p.mode_read) {
		p.mode_read = true;
		char v[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.dxr.lift.probe", v) > 0) {
			if (strcmp(v, "weave") == 0) {
				p.mode = Mode::Weave;
			} else if (strcmp(v, "explicit") == 0) {
				p.mode = Mode::Explicit;
			} else if (strcmp(v, "both") == 0) {
				p.mode = Mode::Both;
			}
		}
		char n[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.dxr.lift.probe_frames", n) > 0 && n[0] != '\0') {
			const long long f = atoll(n);
			p.max_frames = f > 0 ? (uint64_t)f : p.max_frames;
		}
		if (p.mode != Mode::Off) {
			LOGW("LIFT_PROBE: enabled, mode '%s', %llu explicit frames", v,
			     (unsigned long long)p.max_frames);
		}
	}
	return p.mode != Mode::Off;
}

bool
lift_probe_init(XrInstance instance, XrSession session)
{
	Probe &p = g_probe;
	p.instance = instance;
	p.session = session;
	bool ok = true;
#define GET(name, field)                                                                                               \
	ok = ok &&                                                                                                     \
	     xrGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction *>(&p.field)) == XR_SUCCESS && \
	     p.field != nullptr
	GET(xrGetLiftPropertiesDXR, get_props);
	GET(xrCreateLiftStreamDXR, create);
	GET(xrDestroyLiftStreamDXR, destroy);
	GET(xrSubmitLiftFrameDXR, submit);
	GET(xrAcquireLiftResultDXR, acquire);
	GET(xrGetLiftStreamStatsDXR, stats);
#undef GET
	if (!ok) {
		LOGE("LIFT_PROBE: XR_DXR_lift entry points unavailable — probe off");
		p.mode = Mode::Off;
		return false;
	}
	LOGW("LIFT_PROBE: XR_DXR_lift entry points resolved");
	return true;
}

void
lift_probe_frame(uint64_t frame)
{
	Probe &p = g_probe;
	if (p.mode == Mode::Off || p.get_props == nullptr) {
		return;
	}

	// Properties: cheap, kicks activation; streams may be created while
	// ACTIVATING.
	if (!p.streams_tried || frame % 30 == 0) {
		XrLiftPropertiesDXR props = {};
		props.type = XR_TYPE_LIFT_PROPERTIES_DXR;
		XrResult r = p.get_props(p.session, &props);
		if (r != XR_SUCCESS) {
			LOGE(
			    "LIFT_PROBE: xrGetLiftPropertiesDXR -> %d (an in-process session "
			    "answers "
			    "FEATURE_UNSUPPORTED: the probe needs the IPC path)",
			    (int)r);
			p.mode = Mode::Off;
			return;
		}
		if (props.state != p.last_state) {
			p.last_state = props.state;
			LOGW(
			    "LIFT_PROBE: properties state=%s modes=0x%llx maxStreams=%u "
			    "maxViews=%u backend='%s' "
			    "typicalLatency=%.1f ms",
			    state_name(props.state), (unsigned long long)props.supportedModes, props.maxStreams,
			    props.maxViews, props.backend, props.typicalLatency / 1e6);
		}
		if (!p.streams_tried && props.state != XR_LIFT_STATE_UNAVAILABLE_DXR) {
			p.streams_tried = true;
			if (p.mode == Mode::Weave || p.mode == Mode::Both) {
				p.weave_stream = make_stream("weave rect 0");
			}
			if (p.mode == Mode::Explicit || p.mode == Mode::Both) {
				p.explicit_stream = make_stream("explicit");
			}
		}
	}

	explicit_frame(frame);

	if (frame % 120 == 0) {
		log_stats("weave", p.weave_stream);
		if (!p.summary_done) {
			log_stats("explicit", p.explicit_stream);
		}
	}
}

XrLiftStreamDXR
lift_probe_weave_stream()
{
	return g_probe.weave_stream;
}

void
lift_probe_shutdown()
{
	Probe &p = g_probe;
	if (p.destroy != nullptr) {
		if (p.weave_stream != XR_NULL_HANDLE) {
			p.destroy(p.weave_stream);
		}
		if (p.explicit_stream != XR_NULL_HANDLE) {
			p.destroy(p.explicit_stream);
		}
	}
	p.weave_stream = XR_NULL_HANDLE;
	p.explicit_stream = XR_NULL_HANDLE;
	if (p.in_ahb != nullptr) {
		AHardwareBuffer_release(p.in_ahb);
		p.in_ahb = nullptr;
	}
	if (p.out_ahb != nullptr) {
		AHardwareBuffer_release(p.out_ahb);
		p.out_ahb = nullptr;
	}
}
