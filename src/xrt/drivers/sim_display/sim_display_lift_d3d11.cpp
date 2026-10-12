// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE lift module (see sim_display_lift_d3d11.h).
 *
 * DEPRECATED (ADR-053): a thin DP-slot wrapper over the synthesis shared with
 * the DisplayXR-SimConversion provider (src/xrt/drivers/sim_conversion/
 * sim_conv_synth.h). It owns only the per-stream D3D11 textures and the
 * xrt_dp_lift_* plumbing; the pictures come from sim_conv_d3d11_synth_*.
 * Deleted once the lift thread runs on the conversion core (#2003).
 * @ingroup drv_sim_display
 */

#include "sim_display_lift_d3d11.h"
#include "sim_conv_fake_ply.h"
#include "sim_conv_synth.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

DEBUG_GET_ONCE_BOOL_OPTION(sim_display_fake_lift, "SIM_DISPLAY_FAKE_LIFT", false)
DEBUG_GET_ONCE_NUM_OPTION(sim_display_fake_lift_latency_ms, "SIM_DISPLAY_FAKE_LIFT_LATENCY_MS", 8)

//! Whole-field struct_size coverage: a caller built against an OLDER header
//! passes a shorter struct, and the fake writes / reads only what it covers
//! (roadmap conversion-providers.md §10 — the fake used to refuse them).
#define FAKE_COVERS(PTR, TYPE, FIELD) ((PTR)->struct_size >= offsetof(TYPE, FIELD) + sizeof(((TYPE *)0)->FIELD))

#define SIM_FAKE_LIFT_MAX_STREAMS 8
#define SIM_FAKE_LIFT_MAX_VIEWS 8

struct fake_stream
{
	bool used;
	uint64_t id;
	uint32_t mode;
	ID3D11Texture2D *out;
	ID3D11RenderTargetView *rtv;
	uint32_t out_w, out_h;
	DXGI_FORMAT out_fmt;
	std::vector<uint8_t> blob;
	// XR_DXR_lift v3 auxiliary depth (SBS / NVIEW streams that asked for it).
	bool aux_depth;
	ID3D11Texture2D *depth;
	ID3D11RenderTargetView *depth_rtv;
	uint32_t depth_w, depth_h;
	bool depth_ready;  //!< the last convert rendered a depth map
	float convergence; //!< xrt_dp_lift_params::convergence of the last convert
	uint64_t frames;
};

struct sim_fake_lift
{
	ID3D11Device *device;
	struct sim_conv_d3d11_synth *synth;
	uint64_t next_id;
	fake_stream streams[SIM_FAKE_LIFT_MAX_STREAMS];
};

template <typename T>
static void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

static void
stream_release(fake_stream &s)
{
	safe_release(s.rtv);
	safe_release(s.out);
	safe_release(s.depth_rtv);
	safe_release(s.depth);
	s.blob.clear();
	s.blob.shrink_to_fit();
	s.used = false;
}

static fake_stream *
find_stream(struct sim_fake_lift *fl, uint64_t id)
{
	for (auto &s : fl->streams) {
		if (s.used && s.id == id) {
			return &s;
		}
	}
	return nullptr;
}

static void
fake_latency(void)
{
	int64_t ms = debug_get_num_option_sim_display_fake_lift_latency_ms();
	if (ms > 0) {
		os_nanosleep(ms * 1000 * 1000);
	}
}

extern "C" bool
sim_fake_lift_enabled(void)
{
	return debug_get_bool_option_sim_display_fake_lift();
}

extern "C" struct sim_fake_lift *
sim_fake_lift_create(void *d3d11_device)
{
	if (!sim_fake_lift_enabled() || d3d11_device == nullptr) {
		return nullptr;
	}
	auto *fl = new sim_fake_lift{};
	fl->device = static_cast<ID3D11Device *>(d3d11_device);
	fl->device->AddRef();

	fl->synth = sim_conv_d3d11_synth_create(d3d11_device);
	const bool ok = fl->synth != nullptr;
	if (!ok) {
		U_LOG_E("sim_display fake lift: init failed — lift stays unavailable");
		sim_fake_lift_destroy(fl);
		return nullptr;
	}
	U_LOG_W(
	    "sim_display: FAKE lift module active (SIM_DISPLAY_FAKE_LIFT) — shifted SBS/N-view, gradient "
	    "depth, two-layer PLY; not a model");
	static bool s_deprecation_warned = false;
	if (!s_deprecation_warned) {
		s_deprecation_warned = true;
		U_LOG_W(
		    "sim_display: SIM_DISPLAY_FAKE_LIFT is DEPRECATED and is removed one release after the "
		    "conversion core lands; use the DisplayXR-SimConversion conversion provider instead "
		    "(setting ENABLE=1, env DXR_CONV_SIM_CONVERSION_ENABLE=1; ADR-053)");
	}
	return fl;
}

extern "C" void
sim_fake_lift_destroy(struct sim_fake_lift *fl)
{
	if (fl == nullptr) {
		return;
	}
	for (auto &s : fl->streams) {
		stream_release(s);
	}
	sim_conv_d3d11_synth_destroy(fl->synth);
	fl->synth = nullptr;
	safe_release(fl->device);
	delete fl;
}

extern "C" bool
sim_fake_lift_get_caps(struct sim_fake_lift *fl, struct xrt_dp_lift_caps *out)
{
	// The original v1 block (through backend) is required; appended fields are
	// written only when the caller's struct_size covers them.
	if (fl == nullptr || out == nullptr || !FAKE_COVERS(out, struct xrt_dp_lift_caps, backend)) {
		return false;
	}
	out->modes =
	    XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW | XRT_DP_LIFT_MODE_GAUSSIANS;
	out->max_streams = SIM_FAKE_LIFT_MAX_STREAMS;
	out->max_views = SIM_FAKE_LIFT_MAX_VIEWS;
	out->depth_semantics = XRT_DP_LIFT_DEPTH_RELATIVE;
	out->state = XRT_DP_LIFT_STATE_READY;
	// The fake's auxiliary depth is a synthetic METRIC ramp (1 m .. 2 m).
	if (FAKE_COVERS(out, struct xrt_dp_lift_caps, aux_depth_semantics)) {
		out->aux_outputs = XRT_DP_LIFT_AUX_DEPTH;
		out->aux_depth_semantics = XRT_DP_LIFT_DEPTH_METRIC;
	}
	out->typical_latency_ns = (uint64_t)debug_get_num_option_sim_display_fake_lift_latency_ms() * 1000000ull;
	snprintf(out->backend, sizeof(out->backend), "sim_display-fake");
	return true;
}

extern "C" bool
sim_fake_lift_stream_create(struct sim_fake_lift *fl, const struct xrt_dp_lift_stream_info *info, uint64_t *out_id)
{
	if (fl == nullptr || info == nullptr || out_id == nullptr) {
		return false;
	}
	const uint32_t m = info->mode;
	if (m != XRT_DP_LIFT_MODE_DEPTH && m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW &&
	    m != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	for (auto &s : fl->streams) {
		if (!s.used) {
			s = fake_stream{};
			s.used = true;
			s.id = ++fl->next_id;
			s.mode = m;
			s.aux_depth = (m == XRT_DP_LIFT_MODE_SBS || m == XRT_DP_LIFT_MODE_NVIEW) &&
			              info->struct_size >= offsetof(struct xrt_dp_lift_stream_info, aux_depth_format) +
			                                       sizeof(uint32_t) &&
			              (info->aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0;
			*out_id = s.id;
			return true;
		}
	}
	return false;
}

extern "C" void
sim_fake_lift_stream_destroy(struct sim_fake_lift *fl, uint64_t id)
{
	if (fl == nullptr) {
		return;
	}
	fake_stream *s = find_stream(fl, id);
	if (s != nullptr) {
		stream_release(*s);
	}
}

static bool
ensure_out(struct sim_fake_lift *fl, fake_stream &s, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (s.out != nullptr && s.out_w == w && s.out_h == h && s.out_fmt == fmt) {
		return true;
	}
	safe_release(s.rtv);
	safe_release(s.out);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(fl->device->CreateTexture2D(&td, nullptr, &s.out)) ||
	    FAILED(fl->device->CreateRenderTargetView(s.out, nullptr, &s.rtv))) {
		safe_release(s.rtv);
		safe_release(s.out);
		return false;
	}
	s.out_w = w;
	s.out_h = h;
	s.out_fmt = fmt;
	return true;
}

static bool
fake_ensure_depth(struct sim_fake_lift *fl, fake_stream &s, uint32_t w, uint32_t h)
{
	if (s.depth != nullptr && s.depth_w == w && s.depth_h == h) {
		return true;
	}
	safe_release(s.depth_rtv);
	safe_release(s.depth);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(fl->device->CreateTexture2D(&td, nullptr, &s.depth)) ||
	    FAILED(fl->device->CreateRenderTargetView(s.depth, nullptr, &s.depth_rtv))) {
		safe_release(s.depth_rtv);
		safe_release(s.depth);
		return false;
	}
	s.depth_w = w;
	s.depth_h = h;
	return true;
}

extern "C" bool
sim_fake_lift_get_depth(struct sim_fake_lift *fl, uint64_t id, void *d3d11_context, struct xrt_dp_lift_depth *out)
{
	(void)d3d11_context;
	// Everything this writes is in the original block (through vendor_frame_id);
	// the appended relief fields are left to the caller's zero-fill.
	if (fl == nullptr || out == nullptr || !FAKE_COVERS(out, struct xrt_dp_lift_depth, vendor_frame_id)) {
		return false;
	}
	fake_stream *s = find_stream(fl, id);
	if (s == nullptr || !s->aux_depth || !s->depth_ready || s->depth == nullptr) {
		return false;
	}
	out->format = (uint32_t)DXGI_FORMAT_R32_FLOAT;
	out->resource = s->depth;
	out->width = s->depth_w;
	out->height = s->depth_h;
	out->units = XRT_DP_LIFT_DEPTH_METRIC;
	out->encoding = XRT_DP_LIFT_DEPTH_ENCODING_LINEAR;
	out->value_scale = 1.0f; // sample = v in [0, 1] -> 1 m (top) .. 2 m (bottom)
	out->value_offset = 1.0f;
	out->source_width = s->depth_w;
	out->source_height = s->depth_h;
	const float f = 0.78f * (float)(s->depth_w > s->depth_h ? s->depth_w : s->depth_h);
	out->focal_x_px = f;
	out->focal_y_px = f;
	out->principal_x_px = 0.5f * (float)s->depth_w;
	out->principal_y_px = 0.5f * (float)s->depth_h;
	out->near_depth = 1.0f;
	out->far_depth = 2.0f;
	// Relative convergence c in [0, 1] over the 1..2 m range; AUTO = mid.
	out->convergence_depth = s->convergence < 0.0f ? 1.5f : 1.0f + (s->convergence > 1.0f ? 1.0f : s->convergence);
	out->same_inference = 1;
	out->vendor_frame_id = s->frames;
	return true;
}

extern "C" bool
sim_fake_lift_convert(struct sim_fake_lift *fl,
                      uint64_t id,
                      void *d3d11_context,
                      void *input_resource,
                      uint32_t w,
                      uint32_t h,
                      const struct xrt_dp_lift_params *p,
                      const float *viewpoints_xyz,
                      uint32_t viewpoint_floats,
                      void **out_resource,
                      uint32_t *out_w,
                      uint32_t *out_h,
                      uint32_t *out_format)
{
	if (fl == nullptr || d3d11_context == nullptr || input_resource == nullptr || w == 0 || h == 0 ||
	    out_resource == nullptr || out_w == nullptr || out_h == nullptr || out_format == nullptr) {
		return false;
	}
	fake_stream *s = find_stream(fl, id);
	if (s == nullptr || s->mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	uint32_t views = 1;
	if (s->mode == XRT_DP_LIFT_MODE_SBS) {
		views = 2;
	} else if (s->mode == XRT_DP_LIFT_MODE_NVIEW) {
		views = (p != nullptr && p->view_count >= 1) ? p->view_count : 4;
		views = views > SIM_FAKE_LIFT_MAX_VIEWS ? SIM_FAKE_LIFT_MAX_VIEWS : views;
	}
	const bool depth = s->mode == XRT_DP_LIFT_MODE_DEPTH;
	const DXGI_FORMAT fmt = depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
	const uint32_t ow = w * views;
	if (!ensure_out(fl, *s, ow, h, fmt)) {
		return false;
	}

	ID3D11ShaderResourceView *srv = nullptr;
	if (!depth) {
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(fl->device->CreateShaderResourceView(static_cast<ID3D11Resource *>(input_resource), &sd,
		                                                &srv))) {
			return false;
		}
	}

	// Per-view shifts from THIS view's viewpoint when the runtime sent them and
	// the rect width is known (roadmap §10: the fake used to ignore them and
	// draw a fixed ~2 %-per-view fan); the historical fan otherwise.
	bool ok = true;
	if (depth) {
		ok = sim_conv_d3d11_synth_depth(fl->synth, ctx, s->rtv, w, h);
	} else {
		const float strength = p != nullptr ? p->strength : 1.0f;
		const float rect_w =
		    p != nullptr && FAKE_COVERS(p, struct xrt_dp_lift_params, rect_width_m) ? p->rect_width_m : 0.0f;
		float shifts[SIM_CONV_SYNTH_MAX_VIEWS];
		sim_conv_synth_view_shifts(views, strength, viewpoints_xyz, viewpoint_floats / 3u, rect_w, shifts);
		ok = sim_conv_d3d11_synth_views(fl->synth, ctx, srv, s->rtv, w, h, views, shifts);
	}

	// XR_DXR_lift v3: the auxiliary depth of the SAME (fake) inference — the
	// ramp the DEPTH mode draws, at input size, R32F; decoded as 1 + v metres.
	s->depth_ready = false;
	if (ok && s->aux_depth && !depth && fake_ensure_depth(fl, *s, w, h) &&
	    sim_conv_d3d11_synth_depth(fl->synth, ctx, s->depth_rtv, w, h)) {
		s->depth_ready = true;
		s->convergence = p != nullptr ? p->convergence : -1.0f;
		s->frames++;
	}
	safe_release(srv);
	if (!ok) {
		return false;
	}

	fake_latency();

	*out_resource = s->out;
	*out_w = ow;
	*out_h = h;
	*out_format = (uint32_t)fmt;
	return true;
}

extern "C" bool
sim_fake_lift_convert_blob(struct sim_fake_lift *fl,
                           uint64_t id,
                           void *d3d11_context,
                           void *input_resource,
                           uint32_t w,
                           uint32_t h,
                           uint32_t *out_format,
                           const void **out_bytes,
                           size_t *out_size)
{
	if (fl == nullptr || d3d11_context == nullptr || input_resource == nullptr || out_format == nullptr ||
	    out_bytes == nullptr || out_size == nullptr) {
		return false;
	}
	fake_stream *s = find_stream(fl, id);
	if (s == nullptr || s->mode != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	auto *ctx = static_cast<ID3D11DeviceContext *>(d3d11_context);

	// Read the photo back (the fake colours its front layer from it).
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	const uint8_t *rgba = nullptr;
	uint32_t pitch = 0;
	D3D11_MAPPED_SUBRESOURCE map = {};
	bool mapped = false;
	if (SUCCEEDED(fl->device->CreateTexture2D(&td, nullptr, &staging))) {
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, static_cast<ID3D11Resource *>(input_resource), 0, &box);
		if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
			mapped = true;
			rgba = static_cast<const uint8_t *>(map.pData);
			pitch = map.RowPitch;
		}
	}

	size_t need = sim_fake_ply_write(nullptr, 0, nullptr, w, h, 0);
	s->blob.resize(need);
	sim_fake_ply_write(s->blob.data(), s->blob.size(), rgba, w, h, pitch);

	if (mapped) {
		ctx->Unmap(staging, 0);
	}
	safe_release(staging);

	// Photo → splats is seconds on a real module; the fake is quick but not free.
	fake_latency();

	*out_format = XRT_DP_LIFT_BLOB_PLY_3DGS;
	*out_bytes = s->blob.data();
	*out_size = s->blob.size();
	return true;
}
