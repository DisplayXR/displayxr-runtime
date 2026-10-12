// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The S7 convergence observer over u_lift_autoconv (see header).
 * @ingroup aux_conversion
 */

#include "conv_observer_autoconv.h"

#include "util/u_lift_autoconv.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

struct autoconv_obs
{
	struct u_lift_autoconv_params params;
	struct u_lift_autoconv_filter_params fparams;
	struct u_lift_autoconv_filter filter;
	struct u_lift_autoconv_estimate last; //!< last measurement (hysteresis + logs)
	struct u_lift_autoconv_grid grid;     //!< ~64 KiB scratch, per observer (no shared static)
	bool locked_logged;
};

static bool
kind_is_inverse(uint32_t kind)
{
	return kind == XRT_CONV_DEPTH_METRIC_INVERSE || kind == XRT_CONV_DEPTH_AFFINE_INVERSE ||
	       kind == XRT_CONV_DEPTH_AFFINE_DISPARITY || kind == XRT_CONV_DEPTH_STEREO_DISPARITY_PX;
}

static void
obs_reset(void *ctx)
{
	struct autoconv_obs *o = (struct autoconv_obs *)ctx;
	u_lift_autoconv_filter_reset(&o->filter);
	memset(&o->last, 0, sizeof(o->last));
	o->locked_logged = false;
}

static void
obs_observe(void *ctx,
            const struct xrt_conv_depth_info *di,
            const struct conv_depth_samples *s,
            const float active[4],
            uint64_t now_ns)
{
	(void)active; // the depth covers exactly what was converted (snapshots are letterbox-cropped first)
	struct autoconv_obs *o = (struct autoconv_obs *)ctx;
	if (s == NULL || s->samples == NULL || s->width == 0 || s->height == 0) {
		return; // GPU-only depth this frame: nothing to measure
	}
	const float scale = di->value_scale != 0.0f ? di->value_scale : 1.0f;
	const bool ok = u_lift_autoconv_grid_from_samples(
	    s->samples, s->width, s->height, s->stride_floats != 0 ? s->stride_floats : s->width,
	    kind_is_inverse(di->kind), scale, di->value_shift, U_LIFT_AUTOCONV_GRID_MAX_W, &o->grid);
	struct u_lift_autoconv_estimate e;
	memset(&e, 0, sizeof(e));
	if (ok) {
		// Prefer the clump this stream already follows (hysteresis), as the
		// lift thread did.
		struct u_lift_autoconv_params ep = o->params;
		ep.prefer_h = (o->last.valid && o->last.peak_h >= 0.0f) ? o->last.peak_h : -1.0f;
		u_lift_autoconv_find_subject(&o->grid, NULL, &ep, &e);
	}
	o->last = e;
	u_lift_autoconv_filter_measure(&o->filter, &o->fparams, &e, now_ns);
}

static bool
obs_value(void *ctx, uint64_t now_ns, float *out_rel01)
{
	struct autoconv_obs *o = (struct autoconv_obs *)ctx;
	float c = -1.0f;
	if (!u_lift_autoconv_filter_step(&o->filter, &o->fparams, now_ns, &c)) {
		return false;
	}
	if (!o->locked_logged) {
		o->locked_logged = true;
		U_LOG_W("[conv] runtime auto-convergence locked at %.3f (subject h=%.3f, confidence %.2f, %u cells)", c,
		        o->last.mode_h, o->last.confidence, o->last.samples);
	}
	*out_rel01 = c;
	return true;
}

static void
obs_destroy(void *ctx)
{
	free(ctx);
}

static const struct conv_observer_ops k_ops = {
    .reset = obs_reset,
    .observe = obs_observe,
    .value = obs_value,
    .destroy = obs_destroy,
};

bool
conv_observer_autoconv_create(struct conv_observer *out)
{
	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (u_lift_autoconv_source_parse(getenv("DXR_LIFT_AUTO_CONVERGENCE")) != U_LIFT_AUTOCONV_SOURCE_RUNTIME) {
		return false; // the module's own auto-convergence (AUTO goes through)
	}
	struct autoconv_obs *o = (struct autoconv_obs *)calloc(1, sizeof(*o));
	if (o == NULL) {
		return false;
	}
	u_lift_autoconv_params_default(&o->params);
	u_lift_autoconv_filter_params_default(&o->fparams);
	out->ops = &k_ops;
	out->ctx = o;
	return true;
}

bool
conv_observer_autoconv_factory(void *ud, struct conv_observer *out)
{
	(void)ud;
	return conv_observer_autoconv_create(out);
}
