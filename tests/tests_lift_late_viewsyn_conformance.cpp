// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-058 conformance (ADR-054 Amendment 1's tracking -> conversion
 *         -> weave statement), headless, on the CPU reference.
 *
 * A provider produces a result every 15th display frame (4 fps against a
 * 60 Hz display), each one 15 frames (250 ms) after the frame whose eyes S3
 * snapped for it, so a shown result is 250-483 ms old — the shape of the open provider on Apple Silicon (264–386
 * ms per composed frame). A scripted eye path runs for two seconds. For every
 * display frame that has a result to show, three pictures of the SAME latest
 * content are compared:
 *
 *   GT    direct synthesis at this frame's own eyes;
 *   (i)   today: the result as converted, placed for its conversion eyes;
 *   (ii)  ADR-058: late synthesis of the latest image + depth at this
 *         frame's eyes (u_lift_reproject_render).
 *
 * Asserted, on every frame within the 0.10 m correction limit (frames past
 * it are eased toward the plane on purpose and only counted): (ii) matches GT (it is the same synthesis on the same
 * inputs, so the tolerance is 0 -- what this pins is that the ring's source + the frame's eyes are all late placement
 * needs), and (i) does not whenever the eyes moved in a way the synthesis depends on.
 *
 * Content is static, so every error measured is PLACEMENT error, never
 * content age. What the synthesis depends on (u_lift_reproject.h): both
 * geometries put the source camera at the viewpoints' midpoint and
 * x_s = p' + (E - C) q, so a common translation of the eyes changes nothing
 * except, on the COMPOSED path, through the budget's 1 / distance scaling;
 * a head turn (the eyes' spread and depth offset) changes every view. Runs:
 *
 *   - COMPOSED, sway + lean + head turn: (i) is off whenever the distance or
 *     the turn moved; (ii) is exact.
 *   - COMPOSED, lateral only: (i) is already exact -- pinned, because it
 *     bounds what late placement buys: lateral swim of a lifted result does
 *     not exist in today's geometry.
 *   - DISPLAY (metric depth + depthToDisplay), sway + lean + head turn: only
 *     the turn matters (the metric placement is relative to C, distance
 *     included); (ii) is exact.
 *
 * The per-run numbers (mean / max |error| in 1/255 over RGB of both views,
 * along the path) print on stdout.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "lift_late_scene.hpp"

#include "util/u_lift_depth.h"
#include "util/u_lift_reproject.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

using namespace lift_late;

namespace {

constexpr uint32_t kW = 128, kH = 72;  // one view
constexpr uint32_t kDW = 32, kDH = 18; // depth grid
constexpr int kHz = 60;
constexpr int kPeriod = 15;  // provider: one result per 15 display frames = 4 fps
constexpr int kLatency = 15; // result visible 250 ms after its eyes were snapped
constexpr int kFrames = 2 * kHz;
constexpr double kPi = 3.14159265358979323846;

//! Eyes at time t (rect-relative metres).
using EyePath = std::function<void(double t, float eyes[2][3])>;

struct RunStats
{
	const char *name;
	int frames = 0;
	int moved = 0;       //!< frames whose eyes differ from the result's
	int eased = 0;       //!< frames beyond the 0.10 m limit (disparity eased, not compared)
	double i_mean = 0.0; //!< mean over frames of the per-frame mean error, (i)
	double i_max = 0.0;
	double ii_mean = 0.0;
	double ii_max = 0.0;
	double i_over4_mean = 0.0, i_over4_max = 0.0; //!< share of texels off by > 4/255, (i)
	double max_delta_m = 0.0;
};

/*!
 * One conformance run. @p make_source fills a source whose RECORDED
 * viewpoints are @p eyes (the conversion's S3 snapshot).
 */
RunStats
run(const char *name,
    const EyePath &path,
    const std::function<void(const float eyes[2][3], u_lift_reproject_source *)> &make_source,
    const std::function<bool(const u_lift_reproject_delta &)> &must_differ)
{
	RunStats rs;
	rs.name = name;
	u_lift_reproject_policy pol;
	u_lift_reproject_policy_defaults(&pol);
	const size_t n = (size_t)2 * kW * kH * 4;
	std::vector<float> today(n), late(n), gt(n);
	int today_for = -1;
	double i_sum = 0.0, ii_sum = 0.0;

	for (int f = kLatency; f < kFrames; f++) {
		// The provider: conversions start at multiples of kPeriod; the newest
		// one finished by frame f is the one that started kLatency earlier.
		const int r = ((f - kLatency) / kPeriod) * kPeriod;
		float e_r[2][3], e_f[2][3];
		path((double)r / kHz, e_r);
		path((double)f / kHz, e_f);

		u_lift_reproject_source src{};
		make_source(e_r, &src);
		if (today_for != r) {
			// (i) what the conversion thread published: views at e_r.
			u_lift_reproject_decision d0;
			REQUIRE(u_lift_reproject_render(&pol, &src, e_r, 2, kW, kH, today.data(), &d0) ==
			        U_LIFT_REPROJECT_PLACE);
			today_for = r;
		}
		// GT: the same content synthesized directly at this frame's eyes,
		// from a source recorded AT those eyes (no late step involved).
		u_lift_reproject_source direct{};
		make_source(e_f, &direct);
		u_lift_reproject_decision dg;
		REQUIRE(u_lift_reproject_render(&pol, &direct, e_f, 2, kW, kH, gt.data(), &dg) ==
		        U_LIFT_REPROJECT_PLACE);
		REQUIRE(dg.delta.max_m == 0.0f);
		// (ii) late synthesis of the ring's source at this frame's eyes.
		u_lift_reproject_decision dl;
		const u_lift_reproject_verdict v =
		    u_lift_reproject_render(&pol, &src, e_f, 2, kW, kH, late.data(), &dl);
		REQUIRE(v != U_LIFT_REPROJECT_HOLD);
		rs.max_delta_m = dl.delta.max_m > rs.max_delta_m ? dl.delta.max_m : rs.max_delta_m;
		if (v == U_LIFT_REPROJECT_EASE) {
			// Beyond the limit the policy flattens on purpose: not a placement
			// to compare with the direct synthesis.
			CHECK(dl.delta.max_m > pol.max_m);
			rs.eased++;
			continue;
		}

		const Err ei = diff(today, gt);
		const Err eii = diff(late, gt);
		rs.frames++;
		i_sum += ei.mean;
		rs.i_over4_mean += ei.over4;
		rs.i_over4_max = ei.over4 > rs.i_over4_max ? ei.over4 : rs.i_over4_max;
		ii_sum += eii.mean;
		rs.i_max = ei.max > rs.i_max ? ei.max : rs.i_max;
		rs.ii_max = eii.max > rs.ii_max ? eii.max : rs.ii_max;

		INFO(name << " frame " << f << " (result of frame " << r << "): delta " << dl.delta.max_m * 100.0
		          << " cm, (i) mean " << ei.mean << " max " << ei.max << ", (ii) mean " << eii.mean << " max "
		          << eii.max);
		CHECK(eii.max == 0.0); // (ii) is the direct synthesis
		if (must_differ(dl.delta)) {
			rs.moved++;
			CHECK(ei.mean > 0.05); // (i) visibly misplaced
		}
	}
	rs.i_mean = rs.frames > 0 ? i_sum / rs.frames : 0.0;
	rs.ii_mean = rs.frames > 0 ? ii_sum / rs.frames : 0.0;
	rs.i_over4_mean = rs.frames > 0 ? rs.i_over4_mean / rs.frames : 0.0;
	std::printf(
	    "[ADR-058 conformance] %-36s %3d frames compared (%3d moved, %2d eased past the limit, max eye "
	    "delta %.1f cm): (i) today mean %.3f max %.1f /255, texels > 4/255 %.2f%% avg %.2f%% worst | "
	    "(ii) late mean %.3f max %.1f /255\n",
	    rs.name, rs.frames, rs.moved, rs.eased, rs.max_delta_m * 100.0, rs.i_mean, rs.i_max,
	    100.0 * rs.i_over4_mean, 100.0 * rs.i_over4_max, rs.ii_mean, rs.ii_max);
	return rs;
}

// A viewer swaying +-6 cm at 0.5 Hz, leaning in and out +-10 cm at 0.35 Hz
// and turning the head +-20 degrees at 0.3 Hz.
void
sway_lean_turn(double t, float e[2][3])
{
	const float c[3] = {(float)(0.06 * std::sin(2.0 * kPi * 0.5 * t)),
	                    (float)(0.01 * std::sin(2.0 * kPi * 0.8 * t)),
	                    (float)(0.60 + 0.10 * std::sin(2.0 * kPi * 0.35 * t))};
	eye_pair_yaw(c, 0.35 * std::sin(2.0 * kPi * 0.3 * t), e);
}

void
lateral_only(double t, float e[2][3])
{
	const float c[3] = {(float)(0.06 * std::sin(2.0 * kPi * 0.5 * t)),
	                    (float)(0.02 * std::sin(2.0 * kPi * 0.8 * t)), 0.60f};
	eye_pair(c, e);
}

//! The eyes' relative change (a head turn) beyond 2 mm.
bool
spread_moved(const u_lift_reproject_delta &d)
{
	double s = 0.0;
	for (int k = 0; k < 3; k++) {
		const double r = (double)d.d_m[1][k] - (double)d.d_m[0][k];
		s += r * r;
	}
	return std::sqrt(s) > 0.002;
}

//! The pair's distance changed beyond 1 cm.
bool
distance_moved(const u_lift_reproject_delta &d)
{
	return std::fabs(0.5 * ((double)d.d_m[0][2] + (double)d.d_m[1][2])) > 0.01;
}

struct Scene
{
	std::vector<float> color = make_color(kW, kH);
	std::vector<float> disp = make_disparity(kDW, kDH);
	std::vector<float> metric = make_metric(kDW, kDH, 1.8f, 2.0f, 2.4f);
};

} // namespace

TEST_CASE("ADR-058 conformance: composed stream, 4 fps provider, sway + lean + turn", "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_COMPOSED;
		src->params = composed_params(eyes, 0.04f);
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.disp.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	// Composed S5 depends on the eyes' distance (the budget) and the turn,
	// not on a common lateral shift.
	auto moved = [](const u_lift_reproject_delta &d) { return distance_moved(d) || spread_moved(d); };
	const RunStats rs = run("composed, sway + lean + turn", sway_lean_turn, make, moved);
	CHECK(rs.moved > rs.frames / 2);
	CHECK(rs.ii_max == 0.0);
	CHECK(rs.i_mean > 0.1);
}

TEST_CASE("ADR-058 conformance: composed stream, lateral motion alone is invariant", "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_COMPOSED;
		src->params = composed_params(eyes, 0.04f);
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.disp.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	auto never = [](const u_lift_reproject_delta &) { return false; };
	const RunStats rs = run("composed, lateral only", lateral_only, make, never);
	CHECK(rs.max_delta_m > 0.02); // the eyes did move
	CHECK(rs.i_mean < 0.01);      // ... and today's result is still right (float rounding of E - C only)
	CHECK(rs.ii_max == 0.0);
}

TEST_CASE("ADR-058 conformance: display-placed metric depth, 4 fps provider, sway + lean + turn",
          "[lift_late][conformance]")
{
	Scene s;
	auto make = [&](const float eyes[2][3], u_lift_reproject_source *src) {
		src->geometry = U_LIFT_REPROJECT_DISPLAY;
		u_conv_viewsyn_params_defaults(&src->params);
		src->params.upsample = false;
		src->params.snap = false;
		src->params.depth.kind = XRT_CONV_DEPTH_METRIC;
		src->params.view_count = 2;
		std::memcpy(src->params.viewpoints_m, eyes, sizeof(float) * 6);
		const float c[3] = {0.5f * (eyes[0][0] + eyes[1][0]), 0.5f * (eyes[0][1] + eyes[1][1]),
		                    0.5f * (eyes[0][2] + eyes[1][2])};
		const float rc[3] = {0.0f, 0.0f, 0.0f}; // the provider built it for these eyes
		src->display.transform_valid =
		    u_lift_depth_to_display(c, rc, kRectW, kRectH, 2.0f, src->display.depth_to_display);
		src->display.rect_w_m = kRectW;
		src->display.rect_h_m = kRectH;
		src->display.near_m = 1.8f;
		src->display.far_m = 2.4f;
		src->color = s.color.data();
		src->color_w = kW;
		src->color_h = kH;
		src->depth = s.metric.data();
		src->depth_w = kDW;
		src->depth_h = kDH;
	};
	// Metric placement is relative to C: only the turn changes the views.
	const RunStats rs = run("display (metric), sway + lean + turn", sway_lean_turn, make, spread_moved);
	CHECK(rs.moved > rs.frames / 2);
	CHECK(rs.ii_max == 0.0);
	CHECK(rs.i_mean > 0.1);
}


/*
 *
 * The same statement through the REAL ring and stage code (ADR-058 phase 1
 * weave step): a conv_core with a depth-only fake provider (a composed
 * VIEWS_MONO weave-rect stream), CPU glue, and the late stage driven exactly
 * as the presenters drive it (pin -> conv_core_stream_late_prepare -> render
 * when the cache rule asks). The ground truth is a second core with late view
 * synthesis OFF converting at every display frame's own eyes (S5 on the
 * conversion thread, today's path); (i) is that core's result for the eyes of
 * the frame the shown result was converted for.
 *
 */

#include "conversion/conv_core.h"
#include "conversion/conv_stream.h"
#include "conv_fake_provider.h"
#include "os/os_time.h"

#include <atomic>
#include <thread>

namespace {

constexpr uint64_t kDp = 0x1000;

bool
core_alloc(void *, const conv_image_request *req, xrt_conv_image *out)
{
	const uint32_t bpp = req->format == XRT_CONV_FMT_R32_FLOAT || req->format == XRT_CONV_FMT_RGBA8_UNORM ? 4u
	                     : req->format == XRT_CONV_FMT_RGBA16_FLOAT                                       ? 8u
	                                                                                                      : 4u;
	const uint64_t size = (uint64_t)req->width * req->height * bpp * (req->array_layers ? req->array_layers : 1);
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	static std::atomic<uint64_t> key{1};
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp;
	out->import_key = key++;
	out->color_space = req->role == 0 ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	return true;
}

void
core_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
}

std::atomic<int> g_copies{0};

bool
core_copy(void *, const xrt_conv_image *src, const xrt_conv_image *dst)
{
	if (src->cpu_ptr == nullptr || dst->cpu_ptr == nullptr || src->byte_size != dst->byte_size) {
		return false;
	}
	std::memcpy(dst->cpu_ptr, src->cpu_ptr, (size_t)src->byte_size);
	g_copies++;
	return true;
}

//! CPU images of a request -> the reference render (float RGBA, view-major).
bool
render_request(const conv_viewsyn_request *r, uint32_t view_w, uint32_t view_h, std::vector<float> &out)
{
	const xrt_conv_image *c = r->color, *d = r->depth;
	if (c->cpu_ptr == nullptr || d->cpu_ptr == nullptr || c->format != XRT_CONV_FMT_RGBA8_UNORM ||
	    d->format != XRT_CONV_FMT_R32_FLOAT) {
		return false;
	}
	std::vector<float> color((size_t)c->width * c->height * 4);
	for (uint32_t y = 0; y < c->height; y++) {
		const uint8_t *row = (const uint8_t *)c->cpu_ptr + (size_t)y * c->row_stride;
		for (uint32_t x = 0; x < c->width * 4; x++) {
			color[(size_t)y * c->width * 4 + x] = row[x] / 255.0f;
		}
	}
	u_conv_viewsyn_params p;
	conv_viewsyn_request_to_params(r, &p);
	p.upsample = false;
	p.snap = false;
	out.assign((size_t)r->view_count * view_w * view_h * 4, 0.0f);
	return u_conv_viewsyn_ref_render(&p, color.data(), c->width, c->height, (const float *)d->cpu_ptr, d->width,
	                                 d->height, view_w, view_h, out.data());
}

//! RGBA8 quantisation of a view-major float raster (what an RGBA8 target holds).
std::vector<float>
quantise(const std::vector<float> &v)
{
	std::vector<float> q(v.size());
	for (size_t i = 0; i < v.size(); i++) {
		const float c = v[i] < 0.0f ? 0.0f : (v[i] > 1.0f ? 1.0f : v[i]);
		q[i] = std::floor(c * 255.0f + 0.5f) / 255.0f;
	}
	return q;
}

//! S5 on the conversion thread (the late-OFF core): render into the RGBA8 ROW target.
bool
core_render(void *, const conv_viewsyn_request *r)
{
	const xrt_conv_image *t = r->target;
	const uint32_t n = r->view_count;
	if (t == nullptr || t->cpu_ptr == nullptr || n == 0) {
		return false;
	}
	const uint32_t vw = t->width / n, vh = t->height;
	std::vector<float> v;
	if (!render_request(r, vw, vh, v)) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t y = 0; y < vh; y++) {
			uint8_t *row = (uint8_t *)t->cpu_ptr + (size_t)y * t->row_stride + (size_t)i * vw * 4;
			for (uint32_t x = 0; x < vw * 4; x++) {
				const float c = v[((size_t)i * vh + y) * vw * 4 + x];
				row[x] = (uint8_t)std::floor((c < 0 ? 0 : (c > 1 ? 1 : c)) * 255.0f + 0.5f);
			}
		}
	}
	return true;
}

//! A published result's RGBA8 ROW views as a view-major float raster.
std::vector<float>
views_of(const conv_result &r)
{
	const xrt_conv_image &t = r.views;
	const uint32_t n = r.views_info.view_count, vw = t.width / n, vh = t.height;
	std::vector<float> v((size_t)n * vw * vh * 4);
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t y = 0; y < vh; y++) {
			const uint8_t *row = (const uint8_t *)t.cpu_ptr + (size_t)y * t.row_stride + (size_t)i * vw * 4;
			for (uint32_t x = 0; x < vw * 4; x++) {
				v[((size_t)i * vh + y) * vw * 4 + x] = row[x] / 255.0f;
			}
		}
	}
	return v;
}

struct TestCore
{
	conv_core *c = nullptr;
	uint64_t sid = 0;
	uint64_t last_frame = 0;

	TestCore(conv_fake_provider &f, bool late_off, uint32_t max_edge)
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = core_alloc;
		cfg.alloc.free = core_free;
		cfg.alloc.copy = core_copy;
		cfg.viewsyn.render = core_render;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.rear_cap_m = -1.0f;
		cfg.recenter_off = true; // deterministic S3 across the two cores
		cfg.tick_ms = 10;
		cfg.late_viewsyn_off = late_off;
		cfg.late_viewsyn_max_edge = max_edge;
		REQUIRE(conv_core_create(&cfg, &c) == XRT_SUCCESS);
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(c, &d) >= 0);
		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = XRT_CONV_TASK_VIEWS_MONO;
		sd.outputs = XRT_CONV_OUT_VIEWS;
		sd.view_count = 2;
		sd.views_layout = XRT_CONV_VIEWS_ROW;
		sd.input_kind = XRT_CONV_INPUT_MONO;
		conv_stream_options o{};
		o.weave_rect_source = true;
		o.eyes_dp = kDp;
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(c, 1, &sd, &o, &cr) == XRT_SUCCESS);
		sid = cr.stream_id;
	}
	~TestCore()
	{
		conv_core_destroy(&c);
	}

	void
	eyes(const float e[2][3])
	{
		conv_eyes ce{};
		ce.valid = true;
		ce.is_tracking = true;
		ce.count = 2;
		std::memcpy(ce.xyz, e, sizeof(float) * 6);
		ce.timestamp_ns = os_monotonic_get_ns();
		conv_core_eyes_publish(c, kDp, &ce);
	}

	//! Convert the scene at the eyes published last; wait for the result.
	bool
	convert(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
	{
		conv_submit_layout lay{};
		lay.image_count = 1;
		lay.width[0] = w;
		lay.height[0] = h;
		lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
		lay.color_space[0] = XRT_CONV_CS_SRGB;
		lay.role[0] = XRT_CONV_IMG_COLOR;
		conv_submit_slot slot{};
		if (conv_core_stream_begin_submit(c, sid, &lay, &slot) != XRT_SUCCESS) {
			return false;
		}
		std::memcpy(slot.images[0].cpu_ptr, rgba.data(), rgba.size());
		conv_frame_submit fr{};
		fr.source_time = (int64_t)os_monotonic_get_ns();
		fr.rect_valid = true;
		fr.rect_w_m = kRectW;
		fr.rect_h_m = kRectH;
		uint64_t fid = 0;
		if (conv_core_stream_commit_submit(c, sid, slot.slot, &fr, &fid) != XRT_SUCCESS) {
			return false;
		}
		for (int i = 0; i < 3000; i++) {
			conv_result r{};
			if (conv_core_stream_acquire(c, sid, false, &r) == XRT_SUCCESS) {
				const bool done = r.frame_id >= fid;
				conv_core_stream_release(c, sid, r.ring_slot);
				if (done) {
					last_frame = fid;
					return true;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
};

} // namespace

TEST_CASE("ADR-058 conformance through the core: ring + late stage at 4 fps vs conversion at every frame",
          "[lift_late][conformance][core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "depth-fake");
	conv_fake_cap *cap =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	cap->state = XRT_CONV_STATE_READY;

	// The scene: the self-encoding colour as RGBA8 (the fake's depth is a
	// metric ramp, 1 m at the top to 2 m at the bottom).
	const std::vector<float> cf = make_color(kW, kH);
	std::vector<uint8_t> rgba(cf.size());
	for (size_t i = 0; i < cf.size(); i++) {
		rgba[i] = (uint8_t)std::floor(cf[i] * 255.0f + 0.5f);
	}

	SECTION("late ON: placed at every frame's eyes, equal to converting there")
	{
		TestCore late(f, false, 0);
		TestCore truth(f, true, 0);
		std::vector<float> shown, gt, today, today_rgba;
		conv_late_cache cache{};
		int compared = 0, moved = 0, reruns = 0, eased = 0;
		double i_sum = 0.0, i_max = 0.0, ii_max = 0.0;
		const int copies0 = g_copies.load();
		{
			// Warm both cores' policy history (S6 range -> budget / convergence
			// follow the previous result) so the comparison is placement only.
			float e0[2][3];
			sway_lean_turn(0.0, e0);
			for (int k = 0; k < 3; k++) {
				late.eyes(e0);
				REQUIRE(late.convert(rgba, kW, kH));
				truth.eyes(e0);
				REQUIRE(truth.convert(rgba, kW, kH));
			}
		}
		double ii_mean_max = 0.0;
		for (int fr = kLatency; fr < kFrames; fr++) {
			const int r = ((fr - kLatency) / kPeriod) * kPeriod;
			float e_r[2][3], e_f[2][3];
			sway_lean_turn((double)r / kHz, e_r);
			sway_lean_turn((double)fr / kHz, e_f);
			if ((fr - kLatency) % kPeriod == 0) {
				// The provider's result for frame r lands now (S3 saw e_r).
				late.eyes(e_r);
				REQUIRE(late.convert(rgba, kW, kH));
				// (i): today's result for frame r, from the late-OFF core.
				truth.eyes(e_r);
				REQUIRE(truth.convert(rgba, kW, kH));
				conv_result tr{};
				REQUIRE(conv_core_stream_acquire(truth.c, truth.sid, false, &tr) == XRT_SUCCESS);
				REQUIRE_FALSE(tr.late);
				REQUIRE((tr.written & XRT_CONV_OUT_VIEWS) != 0);
				today = views_of(tr);
				conv_core_stream_release(truth.c, truth.sid, tr.ring_slot);
			}
			// GT: today's path converting at THIS frame's eyes.
			truth.eyes(e_f);
			REQUIRE(truth.convert(rgba, kW, kH));
			conv_result tg{};
			REQUIRE(conv_core_stream_acquire(truth.c, truth.sid, false, &tg) == XRT_SUCCESS);
			gt = views_of(tg);
			conv_core_stream_release(truth.c, truth.sid, tg.ring_slot);

			// (ii): the presenter at this frame -- pin, prepare, render if asked.
			late.eyes(e_f);
			conv_result lr{};
			REQUIRE(conv_core_stream_acquire(late.c, late.sid, false, &lr) == XRT_SUCCESS);
			REQUIRE(lr.late);
			CHECK((lr.written & XRT_CONV_OUT_VIEWS) == 0u); // no views on the conversion thread
			conv_late_frame lf;
			REQUIRE(conv_core_stream_late_prepare(late.c, late.sid, lr.ring_slot, nullptr,
			                                      os_monotonic_get_ns(), &cache, &lf));
			CHECK(lf.view_w == kW);
			CHECK(lf.view_h == kH);
			if (lf.rerun) {
				reruns++;
				REQUIRE(render_request(&lf.req, lf.view_w, lf.view_h, shown));
				shown = quantise(shown);
			}
			conv_core_stream_release(late.c, late.sid, lr.ring_slot);
			if (lf.verdict == U_LIFT_REPROJECT_EASE) {
				eased++;
				continue;
			}
			const Err ei = diff(today, gt), eii = diff(shown, gt);
			compared++;
			i_sum += ei.mean;
			i_max = ei.max > i_max ? ei.max : i_max;
			ii_max = eii.max > ii_max ? eii.max : ii_max;
			INFO("frame " << fr << " result of " << r << ": rerun " << lf.rerun << " (i) " << ei.mean
			              << " (ii) " << eii.mean << " max " << eii.max);
			// Within the re-run thresholds the cached views stand in: they
			// may differ from GT by what 0.5 mm of spread / 5 mm of distance
			// moves (a few silhouette texels), never more.
			CHECK(eii.mean < 0.25);
			ii_mean_max = eii.mean > ii_mean_max ? eii.mean : ii_mean_max;
			if (lf.rerun) {
				CHECK(eii.max == 0.0); // freshly placed: exactly the conversion at these eyes
			}
			if (ei.mean > 0.05) {
				moved++;
			}
		}
		std::printf(
		    "[ADR-058 conformance] through the core (ring + late stage), sway + lean + turn: %d frames "
		    "compared, %d eased, %d re-synthesized (%d skipped by the cache rule): (i) today mean %.3f "
		    "max %.1f /255 (%d frames visibly off) | (ii) late: 0 on re-synthesized frames, worst cached "
		    "frame mean %.3f max %.1f /255\n",
		    compared, eased, reruns, compared + eased - reruns, i_sum / (compared > 0 ? compared : 1), i_max,
		    moved, ii_mean_max, ii_max);
		CHECK(compared > 50);
		CHECK(moved > compared / 4);      // today's placement is visibly wrong on head turns
		CHECK(reruns < compared + eased); // the cache rule skips frames
		CHECK(g_copies.load() > copies0); // the late core kept its sources
	}

	SECTION("capped size and the opt-out")
	{
		TestCore capped(f, false, 64);
		const float c[3] = {0.0f, 0.0f, 0.6f};
		float e[2][3];
		eye_pair(c, e);
		capped.eyes(e);
		REQUIRE(capped.convert(rgba, kW, kH));
		conv_result r{};
		REQUIRE(conv_core_stream_acquire(capped.c, capped.sid, false, &r) == XRT_SUCCESS);
		REQUIRE(r.late);
		conv_late_cache cache{};
		conv_late_frame lf;
		REQUIRE(conv_core_stream_late_prepare(capped.c, capped.sid, r.ring_slot, nullptr, os_monotonic_get_ns(),
		                                      &cache, &lf));
		CHECK(lf.view_w == 64u);
		CHECK(lf.view_h == 36u);
		CHECK(lf.rerun);
		// Same eyes again: the cache rule keeps the views.
		REQUIRE(conv_core_stream_late_prepare(capped.c, capped.sid, r.ring_slot, nullptr, os_monotonic_get_ns(),
		                                      &cache, &lf));
		CHECK_FALSE(lf.rerun);
		// A common lateral move: still nothing to do (the geometry is invariant).
		float moved[2][3];
		std::memcpy(moved, e, sizeof(moved));
		moved[0][0] += 0.03f;
		moved[1][0] += 0.03f;
		capped.eyes(moved);
		REQUIRE(conv_core_stream_late_prepare(capped.c, capped.sid, r.ring_slot, nullptr, os_monotonic_get_ns(),
		                                      &cache, &lf));
		CHECK_FALSE(lf.rerun);
		// A head turn: re-synthesize.
		float turned[2][3];
		eye_pair_yaw(c, 0.2, turned);
		capped.eyes(turned);
		REQUIRE(conv_core_stream_late_prepare(capped.c, capped.sid, r.ring_slot, nullptr, os_monotonic_get_ns(),
		                                      &cache, &lf));
		CHECK(lf.rerun);
		conv_core_stream_release(capped.c, capped.sid, r.ring_slot);

		// DXR_LIFT_LATE_VIEWSYN=0: views on the conversion thread, as before.
		TestCore off(f, true, 0);
		off.eyes(e);
		REQUIRE(off.convert(rgba, kW, kH));
		REQUIRE(conv_core_stream_acquire(off.c, off.sid, false, &r) == XRT_SUCCESS);
		CHECK_FALSE(r.late);
		CHECK((r.written & XRT_CONV_OUT_VIEWS) != 0u);
		CHECK_FALSE(conv_core_stream_late_prepare(off.c, off.sid, r.ring_slot, nullptr, os_monotonic_get_ns(),
		                                          &cache, &lf));
		conv_core_stream_release(off.c, off.sid, r.ring_slot);
	}
	conv_fake_fini(&f);
}
