// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion pipeline composer and per-frame execution (S4, S6, S5).
 * @ingroup aux_conversion
 */

#include "conv_pipeline.h"

#include <math.h>
#include <string.h>

bool
conv_task_produces_views(uint32_t task)
{
	return task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH;
}

static uint32_t
primary_output(uint32_t task)
{
	switch (task) {
	case XRT_CONV_TASK_DEPTH_MONO:
	case XRT_CONV_TASK_DEPTH_VIDEO: return XRT_CONV_OUT_DEPTH;
	case XRT_CONV_TASK_DISPARITY_STEREO: return XRT_CONV_OUT_DISPARITY;
	case XRT_CONV_TASK_VIEWS_MONO:
	case XRT_CONV_TASK_VIEWS_FROM_DEPTH: return XRT_CONV_OUT_VIEWS;
	case XRT_CONV_TASK_SPLATS_MONO:
	case XRT_CONV_TASK_SPLATS_STEREO: return XRT_CONV_OUT_SPLAT_BLOB;
	default: return 0;
	}
}

static uint32_t
declared_outputs(const struct xrt_conv_capability *cap, bool composed)
{
	// Composed: the provider's depth feeds the reference view synthesis, so
	// views and the same-inference depth are both available.
	return composed ? (XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH) : cap->outputs;
}

static void
fill_outputs(struct conv_pipeline *p, uint32_t required, const struct xrt_conv_capability *cap)
{
	const uint32_t declared = declared_outputs(cap, p->composed);
	p->outputs = (required | primary_output(p->task)) & (p->provisional ? ~0u : declared);
	if (p->composed) {
		p->provider_outputs = XRT_CONV_OUT_DEPTH;
	} else {
		// Ask for what the stream produces; while provisional, the declared set may still grow.
		p->provider_outputs = p->outputs & (p->provisional ? ~0u : cap->outputs);
	}
	// Observer-only depth (#2003): a view stream whose S7 observers need the
	// same-inference depth asks the provider for it when the capability can
	// produce it (or may, while provisional); S8 never publishes it.
	if (p->observer_depth && !p->composed && (p->outputs & XRT_CONV_OUT_VIEWS) != 0 &&
	    (p->provisional || (cap->outputs & XRT_CONV_OUT_DEPTH) != 0)) {
		p->provider_outputs |= XRT_CONV_OUT_DEPTH;
	}
	p->stages &= ~(CONV_STAGE_S6_DEPTH);
	if ((p->outputs & XRT_CONV_OUT_DEPTH) != 0 || p->composed || (p->provider_outputs & XRT_CONV_OUT_DEPTH) != 0) {
		p->stages |= CONV_STAGE_S6_DEPTH;
	}
}

xrt_result_t
conv_pipeline_compose(const struct conv_pipeline_request *req,
                      const struct xrt_conv_capability *cap,
                      bool composed,
                      bool cap_ready,
                      struct conv_pipeline *out)
{
	if (req == NULL || cap == NULL || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct conv_pipeline p;
	memset(&p, 0, sizeof(p));
	p.task = req->task;
	p.provider_task = cap->task;
	p.composed = composed;
	p.views_layout = req->views_layout;
	p.depth_kind = cap->depth_kind;

	if (conv_task_produces_views(req->task)) {
		const uint32_t n = req->view_count == 0 ? 2u : req->view_count;
		const uint32_t lo = composed ? 2u : (cap->min_views > 0 ? cap->min_views : 1u);
		// #2029: a capability that is not READY may still be describing a
		// conservative max_views (the dp-lift adapter says 2 until its module
		// is up): accept up to XRT_CONV_MAX_VIEWS and let the READY refine
		// decide (conv_pipeline_refine: unsatisfied when it lacks the views).
		const uint32_t hi = composed || !cap_ready ? XRT_CONV_MAX_VIEWS : cap->max_views;
		if (n < lo || n > hi || n > XRT_CONV_MAX_VIEWS) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // G2: maxViews enforced at create
		}
		p.view_count = n;
	}

	const uint32_t declared = declared_outputs(cap, composed);
	const uint32_t missing = req->required_outputs & ~declared;
	if (missing != 0 && cap_ready) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	p.provisional = !cap_ready;

	p.stages = CONV_STAGE_S0_SNAPSHOT | CONV_STAGE_S4_PROVIDER | CONV_STAGE_S8_PUBLISH;
	if (req->weave_rect_source) {
		p.stages |= CONV_STAGE_S1_LETTERBOX;
	}
	if (req->source_format != 0 && cap->input_formats != 0 && req->source_format < 32 &&
	    (cap->input_formats & (1u << req->source_format)) == 0) {
		p.stages |= CONV_STAGE_S2_NORMALISE;
	}
	if (conv_task_produces_views(req->task)) {
		p.stages |= CONV_STAGE_S3_POLICY;
	}
	if (composed) {
		p.stages |= CONV_STAGE_S5_VIEWSYN;
	}
	p.observer_depth = req->observers && conv_task_produces_views(req->task);
	fill_outputs(&p, req->required_outputs, cap);
	if (req->observers && (p.stages & CONV_STAGE_S6_DEPTH) != 0) {
		p.stages |= CONV_STAGE_S7_OBSERVERS;
	}
	*out = p;
	return XRT_SUCCESS;
}

bool
conv_pipeline_refine(struct conv_pipeline *p, uint32_t required_outputs, const struct xrt_conv_capability *cap)
{
	const bool observers = (p->stages & CONV_STAGE_S7_OBSERVERS) != 0;
	p->provisional = false;
	p->depth_kind = cap->depth_kind;
	const uint32_t declared = declared_outputs(cap, p->composed);
	fill_outputs(p, required_outputs, cap);
	if (observers && (p->stages & CONV_STAGE_S6_DEPTH) != 0) {
		p->stages |= CONV_STAGE_S7_OBSERVERS;
	} else {
		p->stages &= ~CONV_STAGE_S7_OBSERVERS;
	}
	// #2029: a stream composed while provisional may ask for more views than
	// the READY capability has: unsatisfied, re-tried on every gate pass.
	bool views_ok = true;
	if (!p->composed && conv_task_produces_views(p->task) && p->view_count != 0) {
		const uint32_t lo = cap->min_views > 0 ? cap->min_views : 1u;
		views_ok = p->view_count >= lo && p->view_count <= cap->max_views;
	}
	return (required_outputs & ~declared) == 0 && views_ok;
}


/*
 *
 * Execution.
 *
 */

static bool
cpu_r32(const struct xrt_conv_image *img)
{
	return img->memory_kind == XRT_CONV_MEM_CPU && img->cpu_ptr != NULL && img->format == XRT_CONV_FMT_R32_FLOAT;
}

static void
run_s6(const struct conv_pipeline *p, const struct conv_depth_readback_ops *readback, struct conv_pipeline_frame *f)
{
	struct xrt_conv_depth_info *di = &f->result.depth_info;
	const struct xrt_conv_image *d = &f->result.depth;
	if (p->composed) {
		// The runtime's view synthesis uses exactly this depth: same inference.
		di->same_inference = 1u;
	}
	if (di->value_scale == 0.0f) {
		di->value_scale = 1.0f;
	}
	memset(&f->samples, 0, sizeof(f->samples));
	f->samples_from_readback = false;
	if (cpu_r32(d)) {
		f->samples.samples = (const float *)d->cpu_ptr;
		f->samples.width = d->width;
		f->samples.height = d->height;
		f->samples.stride_floats = d->row_stride != 0 ? d->row_stride / (uint32_t)sizeof(float) : d->width;
	} else if (readback != NULL && readback->read != NULL) {
		const float *s = NULL;
		uint32_t w = 0, h = 0, stride = 0;
		if (readback->read(readback->ud, d, &s, &w, &h, &stride) && s != NULL) {
			f->samples.samples = s;
			f->samples.width = w;
			f->samples.height = h;
			f->samples.stride_floats = stride;
			f->samples_from_readback = true;
		}
	}
	conv_policy_depth_stats(di, f->samples.samples, f->samples.width, f->samples.height, f->samples.stride_floats);
	if (di->source_width == 0) {
		di->source_width = f->in.image_count > 0 ? f->in.images[0].width : 0;
		di->source_height = f->in.image_count > 0 ? f->in.images[0].height : 0;
	}
}

static bool
run_s5(const struct conv_pipeline *p, const struct conv_viewsyn_ops *viewsyn, struct conv_pipeline_frame *f)
{
	if (viewsyn == NULL || viewsyn->render == NULL || f->in.image_count == 0 || f->views_target.memory_kind == 0) {
		return false;
	}
	struct xrt_conv_depth_info *di = &f->result.depth_info;
	struct conv_viewsyn_request r;
	memset(&r, 0, sizeof(r));
	r.color = &f->in.images[0];
	r.depth = &f->result.depth;
	r.depth_info = di;
	r.view_count = p->view_count;
	memcpy(r.viewpoints_m, f->params.viewpoints_xyz, sizeof(r.viewpoints_m));
	r.rect_width_m = f->params.rect_width_m;
	r.rect_height_m = f->params.rect_height_m;
	if (!conv_policy_depth_range(di, &r.range_near, &r.range_far)) {
		return false; // no range: nothing to normalise against (never a silent min-max)
	}
	if (f->params.convergence_mode == XRT_CONV_CONVERGENCE_DECODED) {
		r.convergence = f->params.convergence;
	} else {
		// AUTO and no estimator: the middle of the content, in nearness.
		const double nn = conv_policy_nearness(di->kind, r.range_near);
		const double nf = conv_policy_nearness(di->kind, r.range_far);
		r.convergence = (float)conv_policy_from_nearness(di->kind, 0.5 * (nn + nf));
	}
	r.budget = f->budget > 0.0f ? f->budget : CONV_POLICY_BUDGET_DEFAULT;
	r.rear_cap_m = f->rear_cap_m;
	r.target = &f->views_target;
	r.layout = p->views_layout;
	r.new_content = f->new_content;
	if (!viewsyn->render(viewsyn->ud, &r)) {
		return false;
	}

	f->result.views = f->views_target;
	f->result.written |= XRT_CONV_OUT_VIEWS;
	struct xrt_conv_views_info *vi = &f->result.views_info;
	memset(vi, 0, sizeof(*vi));
	vi->view_count = p->view_count;
	vi->layout = p->views_layout;
	memcpy(vi->eye_xyz, f->params.viewpoints_xyz, sizeof(vi->eye_xyz));
	vi->baseline_m = f->params.baseline_m;
	di->convergence_value = r.convergence;
	di->suggest_flags |= 4u; // convergence_value valid
	return true;
}

xrt_result_t
conv_pipeline_execute(const struct conv_pipeline *p,
                      const struct conv_viewsyn_ops *viewsyn,
                      const struct conv_depth_readback_ops *readback,
                      struct conv_pipeline_frame *f)
{
	f->provider_out.struct_size = (uint32_t)sizeof(f->provider_out);
	f->provider_out.written = 0;
	f->in.struct_size = (uint32_t)sizeof(f->in);
	f->params.struct_size = (uint32_t)sizeof(f->params);

	// S4: the provider.
	struct conv_host_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.op = CONV_OP_CONVERT;
	cmd.cap_id = f->cap_id;
	cmd.stream = f->provider_stream;
	cmd.frame_in = &f->in;
	cmd.params = &f->params;
	cmd.frame_out = &f->provider_out;
	f->provider_result = conv_host_call(f->host, &cmd);
	f->blocked = cmd.blocked;
	if (f->provider_result != XRT_SUCCESS) {
		return f->provider_result;
	}
	f->result = f->provider_out;

	// S6: depth decode + per-frame range (before S5, which normalises with it).
	if ((p->stages & CONV_STAGE_S6_DEPTH) != 0 && (f->result.written & XRT_CONV_OUT_DEPTH) != 0) {
		run_s6(p, readback, f);
	}

	// S5: the reference view synthesis.
	if ((p->stages & CONV_STAGE_S5_VIEWSYN) != 0) {
		if ((f->result.written & XRT_CONV_OUT_DEPTH) == 0 || !run_s5(p, viewsyn, f)) {
			conv_pipeline_frame_release(readback, f);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
	}

	// Only what the stream produces is published.
	f->result.written &= p->outputs;
	return XRT_SUCCESS;
}

void
conv_pipeline_frame_release(const struct conv_depth_readback_ops *readback, struct conv_pipeline_frame *f)
{
	if (f->samples_from_readback && readback != NULL && readback->release != NULL) {
		readback->release(readback->ud, f->samples.samples);
	}
	f->samples_from_readback = false;
	memset(&f->samples, 0, sizeof(f->samples));
}
