// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The legacy DP-lift adapter (`dp-lift`) — see conv_provider_legacy_dp.h.
 *
 * Pure provider-ABI mapping: no graphics API here (the lift thread behind
 * conv_dp_lift_backend owns the device and the vendor DP), so the adapter's
 * describe / state / G5 rules are host-tested with a fake backend
 * (tests_conv_dp_lift).
 *
 * @ingroup aux_conversion
 */

#include "conv_provider_legacy_dp.h"

#include "conv_policy.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>

// "State is a plain copy": the module-status values ARE the conversion states.
static_assert(XRT_DP_LIFT_MODULE_READY == XRT_CONV_STATE_READY &&
                  XRT_DP_LIFT_MODULE_ACTIVATING == XRT_CONV_STATE_WARMING &&
                  XRT_DP_LIFT_MODULE_ABSENT == XRT_CONV_STATE_ABSENT &&
                  XRT_DP_LIFT_MODULE_TOO_OLD == XRT_CONV_STATE_INCOMPATIBLE &&
                  XRT_DP_LIFT_MODULE_UNLICENSED == XRT_CONV_STATE_UNLICENSED &&
                  XRT_DP_LIFT_MODULE_LOAD_FAILED == XRT_CONV_STATE_FAILED &&
                  XRT_DP_LIFT_MODULE_DISABLED == XRT_CONV_STATE_DISABLED,
              "xrt_dp_lift module_status values must equal XRT_CONV_STATE_*");

namespace {

constexpr uint32_t kMaxStreams = 64;
//! A warm polls the module at most this often while it reports ACTIVATING.
constexpr uint64_t kPollNs = 1000ull * 1000ull * 1000ull;
//! Cancel polling granularity inside a warm (contract: ≤ 250 ms).
constexpr uint64_t kCancelStepNs = 50ull * 1000ull * 1000ull;
//! Warms of the shared module closer together than this (one wave: the
//! prewarm tasks + a create) share one activation attempt. Real retries (the
//! 15 s backoff, an installer's registry change) are far apart.
constexpr uint64_t kRetryDedupeNs = 500ull * 1000ull * 1000ull;

struct dpl_stream
{
	bool used = false;
	uint64_t handle = 0;
	uint32_t cap_id = 0;
	uint32_t mode = 0;  //!< XRT_DP_LIFT_MODE_*
	uint32_t views = 0; //!< SBS 2, NVIEW n, DEPTH 0
	bool want_depth = false;
	uint64_t dp_id = 0;
	uint64_t frames = 0;
	// The module's last output extents for an input of in_w x in_h (output desc).
	uint32_t in_w = 0, in_h = 0;
	uint32_t vw = 0, vh = 0, vf = 0;
	uint32_t dw = 0, dh = 0, df = 0;
};

struct dpl
{
	std::mutex mtx;
	std::condition_variable cv;
	conv_dp_lift_backend be = {};
	bool attached = false;
	uint32_t in_calls = 0;
	dpl_stream streams[kMaxStreams];
	uint64_t next_handle = 0;
	uint64_t generation = 1;
	uint64_t seen_change = 0;
	bool seen_valid = false;
	bool logged_attached = false;
	uint64_t last_attempt_ns = 0; //!< the last activation attempt (warm retries dedupe on it)
};

dpl *
D(xrt_conversion_plugin_instance *inst)
{
	return reinterpret_cast<dpl *>(inst);
}

void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	(void)snprintf(dst, cap, "%s", src != nullptr ? src : "");
}

//! A backend call bracket: false = detached (the caller answers "no module").
bool
enter(dpl *d, conv_dp_lift_backend *out)
{
	std::lock_guard<std::mutex> g(d->mtx);
	if (!d->attached) {
		return false;
	}
	d->in_calls++;
	*out = d->be;
	return true;
}

void
leave(dpl *d)
{
	std::lock_guard<std::mutex> g(d->mtx);
	d->in_calls--;
	d->cv.notify_all();
}

bool
snapshot(dpl *d, conv_dp_lift_snapshot *out)
{
	memset(out, 0, sizeof(*out));
	conv_dp_lift_backend be;
	if (!enter(d, &be)) {
		copy_str(out->detail, sizeof(out->detail), "the D3D11 service compositor is not running");
		return false;
	}
	be.snapshot(be.ud, out);
	leave(d);
	return true;
}

uint32_t
mode_bit_of_cap(uint32_t cap_id)
{
	return cap_id == CONV_DP_LIFT_CAP_VIEWS_MONO ? (XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW)
	                                             : XRT_DP_LIFT_MODE_DEPTH;
}

const char *
mode_name_of_cap(uint32_t cap_id)
{
	return cap_id == CONV_DP_LIFT_CAP_VIEWS_MONO ? "SBS / NVIEW" : "DEPTH";
}

/*!
 * The capability's readiness from a snapshot (spec §4.5: a plain copy of
 * module_status; "not yet asked" is never terminal; "no module" is ABSENT and
 * retryable — the backoff re-warms and re-resolves the factory, G5).
 */
void
state_from_snapshot(bool attached, const conv_dp_lift_snapshot &s, uint32_t cap_id, xrt_conv_state *out)
{
	out->state = XRT_CONV_STATE_ABSENT;
	out->failure_reason = XRT_CONV_FAIL_MODEL;
	out->retryable = 1;
	out->progress = -1.0f;
	if (!attached) {
		copy_str(out->detail, sizeof(out->detail), s.detail);
		return;
	}
	if (!s.attempted) {
		if (s.factory_present) {
			out->state = XRT_CONV_STATE_INSTALLED;
			out->failure_reason = XRT_CONV_FAIL_NONE;
			out->retryable = 0;
			copy_str(out->detail, sizeof(out->detail), "lift factory present; module not instantiated yet");
		} else {
			copy_str(out->detail, sizeof(out->detail), "the display plug-in has no lift factory");
		}
		return;
	}
	if (!s.have_module) {
		copy_str(out->detail, sizeof(out->detail), s.detail[0] != '\0' ? s.detail : "no conversion module");
		return;
	}
	const xrt_dp_lift_caps &c = s.caps;
	uint32_t st = c.module_status;
	if (st == XRT_DP_LIFT_MODULE_UNKNOWN || st > XRT_DP_LIFT_MODULE_DISABLED) {
		st = c.state == XRT_DP_LIFT_STATE_READY        ? XRT_CONV_STATE_READY
		     : c.state == XRT_DP_LIFT_STATE_ACTIVATING ? XRT_CONV_STATE_WARMING
		                                               : XRT_CONV_STATE_ABSENT;
	}
	copy_str(out->version, sizeof(out->version), c.module_version);
	copy_str(out->detail, sizeof(out->detail), c.module_detail);
	out->state = st;
	out->failure_reason = XRT_CONV_FAIL_NONE;
	out->retryable = 0;
	switch (st) {
	case XRT_CONV_STATE_READY:
		if ((c.modes & mode_bit_of_cap(cap_id)) == 0) {
			// The module is up but has no such mode: this capability is absent.
			out->state = XRT_CONV_STATE_ABSENT;
			out->failure_reason = XRT_CONV_FAIL_MODEL;
			out->retryable = 1;
			(void)snprintf(out->detail, sizeof(out->detail), "the conversion module has no %s mode",
			               mode_name_of_cap(cap_id));
		}
		break;
	case XRT_CONV_STATE_WARMING: break;
	case XRT_CONV_STATE_ABSENT:
		out->failure_reason = XRT_CONV_FAIL_MODEL;
		out->retryable = 1;
		break;
	case XRT_CONV_STATE_INCOMPATIBLE: out->failure_reason = XRT_CONV_FAIL_VERSION; break;
	case XRT_CONV_STATE_UNLICENSED: out->failure_reason = XRT_CONV_FAIL_LICENCE; break;
	case XRT_CONV_STATE_FAILED:
		out->failure_reason = XRT_CONV_FAIL_LOAD;
		out->retryable = 1;
		break;
	default: break;
	}
}

uint32_t
max_views_of(const conv_dp_lift_snapshot &s)
{
	if (!s.have_module) {
		return XRT_CONV_MAX_VIEWS; // static describe: refined after the first lift_get_caps
	}
	if ((s.caps.modes & XRT_DP_LIFT_MODE_NVIEW) == 0) {
		return 2;
	}
	uint32_t mv = s.caps.max_views;
	mv = mv < 2 ? 2 : mv;
	return mv > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : mv;
}

/*! The static describe (conv_dp_lift_describe_static, shared with the
 *  headless status), refined from what the live module answered. */
void
fill_cap(const conv_dp_lift_snapshot &s, uint32_t index, xrt_conv_capability *c)
{
	(void)conv_dp_lift_describe_static(index, c);
	if (s.have_module) {
		copy_str(c->backend, sizeof(c->backend), s.caps.backend);
		copy_str(c->model_id, sizeof(c->model_id), s.caps.backend);
		copy_str(c->model_version, sizeof(c->model_version), s.caps.module_version);
		c->max_streams = s.caps.max_streams != 0 ? s.caps.max_streams : CONV_DP_LIFT_DEFAULT_MAX_STREAMS;
		c->est_latency_ms = (uint32_t)(s.caps.typical_latency_ns / 1000000ull);
	}

	if (c->task == XRT_CONV_TASK_VIEWS_MONO) {
		c->max_views = max_views_of(s);
		if (s.has_depth_slot) {
			c->flags |= XRT_CONV_CAP_OFFAXIS_CAMERA; // ADR-048 era: rect + nominal z honoured
		}
		c->outputs = XRT_CONV_OUT_VIEWS;
		// Aux depth: provisional until the module is READY, then iff AUX_DEPTH.
		// A module answers caps while it is still ACTIVATING, and a shipping
		// plug-in sets AUX_DEPTH only once its model is up, so a caps answer
		// taken during warm-up says "no depth" for a module that will have it.
		// Describing from that answer latched every VIEWS|DEPTH stream as
		// unsatisfied for its whole life (#2025, lifted-rect cursor).
		const bool module_ready = s.have_module && s.caps.module_status == XRT_DP_LIFT_MODULE_READY;
		if (!module_ready || (s.caps.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0) {
			c->outputs |= XRT_CONV_OUT_DEPTH;
		}
		c->depth_kind = s.have_module && s.caps.aux_depth_semantics == XRT_DP_LIFT_DEPTH_METRIC
		                    ? XRT_CONV_DEPTH_METRIC
		                    : XRT_CONV_DEPTH_AFFINE_INVERSE;
	} else {
		c->outputs = XRT_CONV_OUT_DEPTH;
		c->flags = c->task == XRT_CONV_TASK_DEPTH_VIDEO ? XRT_CONV_CAP_TEMPORAL : 0u;
		c->depth_kind = s.have_module && s.caps.depth_semantics == XRT_DP_LIFT_DEPTH_METRIC
		                    ? XRT_CONV_DEPTH_METRIC
		                    : XRT_CONV_DEPTH_AFFINE;
	}
}

uint32_t
depth_kind_of(uint32_t units, uint32_t encoding)
{
	const bool metric = units == XRT_DP_LIFT_DEPTH_METRIC;
	const bool inverse = encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE;
	if (metric) {
		return inverse ? XRT_CONV_DEPTH_METRIC_INVERSE : XRT_CONV_DEPTH_METRIC;
	}
	return inverse ? XRT_CONV_DEPTH_AFFINE_INVERSE : XRT_CONV_DEPTH_AFFINE;
}

dpl_stream *
find_stream_locked(dpl *d, uint64_t handle)
{
	for (auto &s : d->streams) {
		if (s.used && s.handle == handle) {
			return &s;
		}
	}
	return nullptr;
}


/*
 *
 * Provider iface.
 *
 */

bool
dpl_get_provider_status(xrt_conv_provider_status *out)
{
	if (out == nullptr) {
		return false;
	}
	out->state = 1u; // XRT_PLUGIN_PLATFORM_STATE_READY: the display plug-in is loaded
	out->flags = 0;
	copy_str(out->hint, sizeof(out->hint), "the display plug-in's lift slots (legacy adapter, in-service)");
	return true;
}

xrt_result_t
dpl_probe(xrt_conversion_plugin_instance **out_inst)
{
	(void)out_inst;
	return XRT_ERROR_PROBER_NOT_SUPPORTED; // built in: created by conv_provider_legacy_dp_create
}

xrt_result_t
dpl_describe_cap(xrt_conversion_plugin_instance *inst, uint32_t index, xrt_conv_capability *out, uint32_t *out_count)
{
	if (out_count != nullptr) {
		*out_count = CONV_DP_LIFT_CAP_COUNT;
	}
	if (out == nullptr) {
		return XRT_SUCCESS;
	}
	if (index >= CONV_DP_LIFT_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	conv_dp_lift_snapshot s;
	(void)snapshot(D(inst), &s);
	fill_cap(s, index, out);
	return XRT_SUCCESS;
}

uint64_t
dpl_get_generation(xrt_conversion_plugin_instance *inst)
{
	dpl *d = D(inst);
	conv_dp_lift_snapshot s;
	const bool attached = snapshot(d, &s);
	std::lock_guard<std::mutex> g(d->mtx);
	// Describe-relevant facts only: a module coming up (or going), its modes,
	// views, aux depth and identity refine the static describe.
	uint64_t key = attached ? 1u : 0u;
	key = key * 31u + (s.have_module ? 1u : 0u);
	key = key * 31u + (s.has_depth_slot ? 1u : 0u);
	if (s.have_module) {
		// READY or not decides whether aux depth is still provisional (fill_cap).
		key = key * 131u + (s.caps.module_status == XRT_DP_LIFT_MODULE_READY ? 1u : 0u);
		key = key * 131u + s.caps.modes;
		key = key * 131u + s.caps.max_views;
		key = key * 131u + s.caps.max_streams;
		key = key * 131u + s.caps.aux_outputs;
		key = key * 131u + s.caps.aux_depth_semantics;
		key = key * 131u + s.caps.depth_semantics;
		for (const char *p = s.caps.backend; *p != '\0'; p++) {
			key = key * 131u + (uint8_t)*p;
		}
		for (const char *p = s.caps.module_version; *p != '\0'; p++) {
			key = key * 131u + (uint8_t)*p;
		}
	}
	if (!d->seen_valid || key != d->seen_change) {
		if (d->seen_valid) {
			d->generation++;
		}
		d->seen_change = key;
		d->seen_valid = true;
	}
	return d->generation;
}

xrt_result_t
dpl_get_state(xrt_conversion_plugin_instance *inst, uint32_t cap_id, xrt_conv_state *out)
{
	if (out == nullptr || cap_id == 0 || cap_id > 3) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	conv_dp_lift_snapshot s;
	const bool attached = snapshot(D(inst), &s);
	const uint32_t size = out->struct_size;
	memset(out, 0, sizeof(*out));
	out->struct_size = size;
	state_from_snapshot(attached, s, cap_id, out);
	return XRT_SUCCESS;
}

xrt_result_t
dpl_warm(xrt_conversion_plugin_instance *inst, uint32_t cap_id, bool (*is_cancelled)(void *ctx), void *cancel_ctx)
{
	dpl *d = D(inst);
	conv_dp_lift_backend be;
	if (!enter(d, &be)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED; // get_state: ABSENT, retryable
	}
	conv_dp_lift_snapshot s;
	be.snapshot(be.ud, &s);
	bool attempt = !s.attempted || !s.have_module;
	if (attempt && s.attempted) {
		// The capabilities share one module: several warms in one wave make
		// one attempt, not one each.
		std::lock_guard<std::mutex> g(d->mtx);
		const uint64_t now = os_monotonic_get_ns();
		attempt = d->last_attempt_ns == 0 || now - d->last_attempt_ns >= kRetryDedupeNs;
	}
	if (attempt) {
		// G5: every attempt re-resolves the factory pair from the plug-in that
		// registered it; "no module" last time is retried, never latched.
		(void)be.activate(be.ud, s.attempted);
		std::lock_guard<std::mutex> g(d->mtx);
		d->last_attempt_ns = os_monotonic_get_ns();
	}
	xrt_result_t r = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	uint64_t last_poll = os_monotonic_get_ns();
	for (;;) {
		be.snapshot(be.ud, &s);
		xrt_conv_state st = {};
		st.struct_size = (uint32_t)sizeof(st);
		state_from_snapshot(true, s, cap_id, &st);
		if (st.state == XRT_CONV_STATE_READY) {
			r = XRT_SUCCESS;
			break;
		}
		if (st.state != XRT_CONV_STATE_WARMING && st.state != XRT_CONV_STATE_INSTALLED) {
			break; // terminal: warm_done reads the reason / retryable from get_state
		}
		if (is_cancelled != nullptr && is_cancelled(cancel_ctx)) {
			break;
		}
		{
			std::lock_guard<std::mutex> g(d->mtx);
			if (!d->attached) {
				break; // detaching: the backend is going away
			}
		}
		os_nanosleep((int64_t)kCancelStepNs);
		const uint64_t now = os_monotonic_get_ns();
		if (now - last_poll >= kPollNs) {
			last_poll = now;
			be.poll(be.ud); // ≤ 1 Hz while ACTIVATING
		}
	}
	leave(d);
	return r;
}

xrt_result_t
dpl_evict(xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	(void)inst;
	(void)cap_id;
	return XRT_SUCCESS; // the legacy module cannot unload; a later warm finds it READY
}

xrt_result_t
dpl_stream_create(xrt_conversion_plugin_instance *inst,
                  uint32_t cap_id,
                  const xrt_conv_stream_info *info,
                  uint64_t *out_stream)
{
	dpl *d = D(inst);
	if (info == nullptr || out_stream == nullptr || cap_id == 0 || cap_id > 3) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	dpl_stream ns;
	ns.cap_id = cap_id;
	xrt_dp_lift_stream_info si = {};
	si.struct_size = (uint32_t)sizeof(si);
	si.input_scale = 1.0f; // the runtime's S0 snapshot already applied any scale
	if (cap_id == CONV_DP_LIFT_CAP_VIEWS_MONO) {
		ns.views = info->view_count != 0 ? info->view_count : 2u;
		ns.mode = ns.views == 2 ? XRT_DP_LIFT_MODE_SBS : XRT_DP_LIFT_MODE_NVIEW;
		ns.want_depth = (info->outputs & XRT_CONV_OUT_DEPTH) != 0;
		si.content_hint = info->content_hint == XRT_CONV_CONTENT_PHOTO ? XRT_DP_LIFT_CONTENT_PHOTO
		                                                               : XRT_DP_LIFT_CONTENT_VIDEO;
		si.aux_outputs = ns.want_depth ? XRT_DP_LIFT_AUX_DEPTH : 0u;
	} else {
		ns.views = 0;
		ns.mode = XRT_DP_LIFT_MODE_DEPTH;
		si.content_hint =
		    cap_id == CONV_DP_LIFT_CAP_DEPTH_MONO ? XRT_DP_LIFT_CONTENT_PHOTO : XRT_DP_LIFT_CONTENT_VIDEO;
	}
	si.mode = ns.mode;

	conv_dp_lift_backend be;
	if (!enter(d, &be)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const xrt_result_t r = be.stream_create(be.ud, &si, &ns.dp_id);
	leave(d);
	if (r != XRT_SUCCESS) {
		return r;
	}
	std::lock_guard<std::mutex> g(d->mtx);
	for (auto &s : d->streams) {
		if (!s.used) {
			s = ns;
			s.used = true;
			s.handle = ++d->next_handle;
			*out_stream = s.handle;
			return XRT_SUCCESS;
		}
	}
	return XRT_ERROR_CLIENT_LIMIT_REACHED; // (the module stream leaks until its DP goes; never reached in practice)
}

xrt_result_t
dpl_stream_get_output_desc(
    xrt_conversion_plugin_instance *inst, uint64_t stream, uint32_t w, uint32_t h, xrt_conv_output_desc *out)
{
	dpl *d = D(inst);
	if (out == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	std::lock_guard<std::mutex> g(d->mtx);
	const dpl_stream *s = find_stream_locked(d, stream);
	if (s == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// The legacy contract returns a texture of the module's choosing: predict
	// today's shape (views side by side at input size, R32F depth at input
	// size), and once a conversion at this input size came back, its extents.
	const bool seen = s->in_w == w && s->in_h == h;
	if (s->mode != XRT_DP_LIFT_MODE_DEPTH) {
		out->views_width = seen && s->vw != 0 ? s->vw : s->views * w;
		out->views_height = seen && s->vh != 0 ? s->vh : h;
		out->views_format = XRT_CONV_FMT_RGBA8_UNORM;
	}
	if (s->mode == XRT_DP_LIFT_MODE_DEPTH || s->want_depth) {
		out->depth_width = seen && s->dw != 0 ? s->dw : w;
		out->depth_height = seen && s->dh != 0 ? s->dh : h;
		out->depth_format = XRT_CONV_FMT_R32_FLOAT;
	}
	return XRT_SUCCESS;
}

xrt_result_t
dpl_stream_convert(xrt_conversion_plugin_instance *inst,
                   uint64_t stream,
                   const xrt_conv_frame_in *in,
                   const xrt_conv_params *params,
                   xrt_conv_frame_out *out)
{
	dpl *d = D(inst);
	if (in == nullptr || params == nullptr || out == nullptr || in->image_count < 1) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	dpl_stream s;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		const dpl_stream *f = find_stream_locked(d, stream);
		if (f == nullptr) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		s = *f;
	}

	// The runtime's decisions in the legacy contract's terms.
	xrt_dp_lift_params lp = {};
	lp.struct_size = (uint32_t)sizeof(lp);
	lp.convergence =
	    params->convergence_mode == CONV_CONVERGENCE_MODE_LEGACY_RELATIVE ? params->convergence : -1.0f;
	lp.strength = params->strength > 0.0f ? params->strength : 1.0f;
	lp.inpaint = (params->frame_flags & XRT_CONV_FRAME_INPAINT) != 0 ? 1u : 0u;
	lp.view_count = s.mode == XRT_DP_LIFT_MODE_NVIEW ? s.views : (s.mode == XRT_DP_LIFT_MODE_SBS ? 2u : 0u);
	lp.focal_px = params->focal_px;
	lp.rect_width_m = params->rect_width_m;
	lp.rect_height_m = params->rect_height_m;
	lp.baseline_m = params->baseline_m;
	lp.axis_mode = params->axis_mode != 0 ? params->axis_mode : XRT_DP_LIFT_AXIS_X;
	lp.max_offset_m = params->max_offset_m;
	lp.viewpoint_frame = XRT_DP_LIFT_VIEWPOINT_FRAME_RECT;
	lp.viewpoint_source = params->viewpoint_source;
	lp.nominal_z_m = params->nominal_z_m;

	// Viewpoints: the legacy contract hands TRACKED / rig conversions the PAIR
	// ("a module spreads N views around it"); the policy stage spread it at its
	// own spacing for N > 2, so give the module the pair back.
	float vps[3 * XRT_CONV_MAX_VIEWS];
	uint32_t n = params->viewpoint_count > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : params->viewpoint_count;
	memcpy(vps, params->viewpoints_xyz, sizeof(float) * 3 * n);
	if (s.mode == XRT_DP_LIFT_MODE_DEPTH) {
		n = 0;
	} else if (n > 2 && params->viewpoint_source != XRT_CONV_VIEWPOINTS_EXPLICIT) {
		float mid[3] = {0, 0, 0}, dlt[3];
		for (uint32_t i = 0; i < n; i++) {
			for (int k = 0; k < 3; k++) {
				mid[k] += params->viewpoints_xyz[i][k] / (float)n;
			}
		}
		for (int k = 0; k < 3; k++) {
			dlt[k] = params->viewpoints_xyz[1][k] - params->viewpoints_xyz[0][k];
			vps[k] = mid[k] - 0.5f * dlt[k];
			vps[3 + k] = mid[k] + 0.5f * dlt[k];
		}
		n = 2;
		lp.baseline_m = std::sqrt(dlt[0] * dlt[0] + dlt[1] * dlt[1] + dlt[2] * dlt[2]);
	}

	conv_dp_lift_convert io = {};
	io.input = &in->images[0];
	io.params = &lp;
	io.viewpoints = n > 0 ? vps : nullptr;
	io.viewpoint_floats = 3 * n;
	io.primary_is_depth = s.mode == XRT_DP_LIFT_MODE_DEPTH;
	io.views_target = s.mode != XRT_DP_LIFT_MODE_DEPTH && out->views.memory_kind != 0 ? &out->views : nullptr;
	io.depth_target = out->depth.memory_kind != 0 ? &out->depth : nullptr;
	io.want_depth = s.mode != XRT_DP_LIFT_MODE_DEPTH && io.depth_target != nullptr;

	conv_dp_lift_backend be;
	if (!enter(d, &be)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	const xrt_result_t r = be.convert(be.ud, s.dp_id, &io);
	leave(d);

	out->written = 0;
	if (r != XRT_SUCCESS || (!io.views_written && !io.depth_written)) {
		return r != XRT_SUCCESS ? r : XRT_ERROR_OUTPUT_REQUEST_FAILURE; // keep the previous result
	}
	const uint32_t w = in->images[0].width, h = in->images[0].height;
	if (io.views_written) {
		out->written |= XRT_CONV_OUT_VIEWS;
		xrt_conv_views_info *vi = &out->views_info;
		memset(vi, 0, sizeof(*vi));
		vi->view_count = lp.view_count;
		vi->layout = XRT_CONV_VIEWS_ROW;
		const uint32_t m =
		    params->viewpoint_count > XRT_CONV_MAX_VIEWS ? XRT_CONV_MAX_VIEWS : params->viewpoint_count;
		memcpy(vi->eye_xyz, params->viewpoints_xyz, sizeof(float) * 3 * m);
		vi->baseline_m = lp.baseline_m;
	}
	if (io.depth_written) {
		out->written |= XRT_CONV_OUT_DEPTH;
		xrt_conv_depth_info *di = &out->depth_info;
		memset(di, 0, sizeof(*di));
		di->invalid_sample = NAN;
		di->measure = XRT_CONV_MEASURE_PLANAR;
		di->source_width = w;
		di->source_height = h;
		di->provider_frame_id = s.frames + 1;
		if (io.primary_is_depth) {
			// A DEPTH-mode map: units from the caps, no encoding / scale info.
			conv_dp_lift_snapshot snap;
			(void)snapshot(d, &snap);
			di->kind = snap.have_module && snap.caps.depth_semantics == XRT_DP_LIFT_DEPTH_METRIC
			               ? XRT_CONV_DEPTH_METRIC
			               : XRT_CONV_DEPTH_AFFINE;
			di->value_scale = 1.0f;
			di->intrinsics_source = XRT_CONV_INTRINSICS_NONE;
			di->same_inference = 1;
			di->temporal = s.cap_id == CONV_DP_LIFT_CAP_DEPTH_VIDEO ? 1u : 0u;
		} else if (io.have_depth_meta) {
			// Field by field (spec §4.5); range_* stays 0 (S6 measures it).
			const xrt_dp_lift_depth &dd = io.depth;
			di->kind = depth_kind_of(dd.units, dd.encoding);
			di->value_scale =
			    dd.value_scale != 0.0f && std::isfinite(dd.value_scale) ? dd.value_scale : 1.0f;
			di->value_shift = std::isfinite(dd.value_offset) ? dd.value_offset : 0.0f;
			di->near_value = dd.near_depth > 0.0f ? dd.near_depth : 0.0f;
			di->far_value = dd.far_depth > 0.0f ? dd.far_depth : 0.0f;
			if (dd.focal_x_px > 0.0f && dd.focal_y_px > 0.0f) {
				di->fx = dd.focal_x_px;
				di->fy = dd.focal_y_px;
				di->cx = dd.principal_x_px;
				di->cy = dd.principal_y_px;
				di->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
			}
			if (dd.source_width != 0) {
				di->source_width = dd.source_width;
				di->source_height = dd.source_height;
			}
			if (dd.convergence_depth > 0.0f) {
				di->convergence_value = dd.convergence_depth;
				di->suggest_flags |= 4u; // convergence_value valid
			}
			if (dd.relief_valid != 0 && std::isfinite(dd.relief_scale) && std::isfinite(dd.relief_offset) &&
			    dd.relief_scale != 0.0f) {
				di->relief_valid = 1;
				di->relief_scale = dd.relief_scale;
				di->relief_offset = dd.relief_offset;
			}
			di->same_inference = dd.same_inference != 0 ? 1u : 0u;
			di->provider_frame_id = dd.vendor_frame_id;
		}
	}

	std::lock_guard<std::mutex> g(d->mtx);
	dpl_stream *f = find_stream_locked(d, stream);
	if (f != nullptr) {
		f->frames++;
		f->in_w = w;
		f->in_h = h;
		if (io.views_written) {
			f->vw = io.out_w;
			f->vh = io.out_h;
			f->vf = io.out_format;
		}
		if (io.depth_written) {
			f->dw = io.primary_is_depth ? io.out_w : io.depth.width;
			f->dh = io.primary_is_depth ? io.out_h : io.depth.height;
			f->df = XRT_CONV_FMT_R32_FLOAT;
		}
	}
	return XRT_SUCCESS;
}

void
dpl_stream_destroy(xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	dpl *d = D(inst);
	uint64_t dp_id = 0;
	{
		std::lock_guard<std::mutex> g(d->mtx);
		dpl_stream *s = find_stream_locked(d, stream);
		if (s == nullptr) {
			return;
		}
		dp_id = s->dp_id;
		*s = dpl_stream{};
	}
	conv_dp_lift_backend be;
	if (enter(d, &be)) {
		be.stream_destroy(be.ud, dp_id);
		leave(d);
	}
}

void
dpl_destroy(xrt_conversion_plugin_instance *inst)
{
	conv_provider_legacy_dp_detach(inst);
	delete D(inst);
}

const xrt_conversion_plugin_iface k_iface = {
    /* struct_size */ (uint32_t)sizeof(xrt_conversion_plugin_iface),
    /* reserved_0 */ 0,
    /* id */ CONV_DP_LIFT_ID,
    /* display_name */ "Display plug-in lift slots (legacy adapter)",
    /* vendor */ nullptr,
    /* version */ nullptr,
    dpl_get_provider_status,
    dpl_probe,
    dpl_describe_cap,
    dpl_get_generation,
    dpl_get_state,
    dpl_warm,
    dpl_evict,
    dpl_stream_create,
    dpl_stream_get_output_desc,
    dpl_stream_convert,
    dpl_stream_destroy,
    nullptr, // job_execute: no job lane (no SPLATS capability)
    dpl_destroy,
};

} // namespace


/*
 *
 * Exported.
 *
 */

extern "C" bool
conv_provider_legacy_dp_create(const struct conv_dp_lift_backend *backend,
                               const struct xrt_conversion_plugin_iface **out_iface,
                               struct xrt_conversion_plugin_instance **out_inst)
{
	if (out_iface == nullptr || out_inst == nullptr) {
		return false;
	}
	auto *d = new dpl();
	if (backend != nullptr && backend->snapshot != nullptr) {
		d->be = *backend;
		d->attached = true;
	}
	*out_iface = &k_iface;
	*out_inst = reinterpret_cast<xrt_conversion_plugin_instance *>(d);
	return true;
}

extern "C" void
conv_provider_legacy_dp_detach(struct xrt_conversion_plugin_instance *inst)
{
	if (inst == nullptr) {
		return;
	}
	dpl *d = D(inst);
	std::unique_lock<std::mutex> lk(d->mtx);
	d->attached = false;
	d->cv.wait(lk, [d] { return d->in_calls == 0; });
	d->be = conv_dp_lift_backend{};
}

extern "C" void
conv_provider_legacy_dp_attach(struct xrt_conversion_plugin_instance *inst, const struct conv_dp_lift_backend *backend)
{
	if (inst == nullptr || backend == nullptr || backend->snapshot == nullptr) {
		return;
	}
	dpl *d = D(inst);
	std::lock_guard<std::mutex> g(d->mtx);
	d->be = *backend;
	d->attached = true;
	// Module streams of the previous backend are gone with it.
	for (auto &s : d->streams) {
		s = dpl_stream{};
	}
}
