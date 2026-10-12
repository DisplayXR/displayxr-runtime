// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 S5 — the reference view synthesis' pure math
 *         (util/u_conv_viewsyn.h), ported case for case from
 *         displayxr-web-lift `test/lift-dibr-math.test.mjs`.
 *
 * Ported cases keep the JS test names. The JS cases NOT ported pin functions
 * this stage deliberately does not contain — they are policy stages owned
 * elsewhere in the runtime (ADR-053 decision 4):
 *   percentileRange (x3), centreMedian (x2), DepthNormalizer (EMA / reset,
 *   dead band), auto-convergence on the synthetic scene, centreMean,
 *   deadBand, thumbSad (scene cuts).
 * The cases after the port pin what the runtime adds: declared depth kinds,
 * viewpoints in metres, convergence in decoded units, the ADR-040 rear cap and
 * the CPU mirror of the shader's march.
 *
 * @ingroup tests
 */

#include "util/u_conv_viewsyn.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

using V3 = u_conv_viewsyn_vec3;

#define NEAR(a, b, eps) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps))

V3
v3(double x, double y, double z)
{
	V3 v = {x, y, z};
	return v;
}

// DisplayXR Browser 1.0.2 panel run (3840x2160 panel, tile 800x450 CSS @ dpr 2.5,
// each view 2000x1125): the two projection matrices VERBATIM (DUMPVIEW 0 / 1).
const float kPanel[2][16] = {
    {6.166205883026123f, 0, 0, 0, 0, 10.96214485168457f, 0, 0, 0.45363613963127136f, -0.8832857012748718f,
     -1.0001999139785767f, -1, 0, 0, -0.2000199854373932f, 0},
    {6.157009124755859f, 0, 0, 0, 0, 10.945794105529785f, 0, 0, -0.2347441464662552f, -0.9125044345855713f,
     -1.0001999139785767f, -1, 0, 0, -0.2000199854373932f, 0},
};
constexpr double kPanelViewH = 1125.0; // px per window height in that run

void
panel_eyes(V3 out[2], double *aspect)
{
	for (int i = 0; i < 2; i++) {
		out[i] = u_conv_viewsyn_eye_from_projection(kPanel[i], aspect);
	}
}

double
sgn(double v)
{
	return v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0);
}

} // namespace

// ---- ported from lift-dibr-math.test.mjs ----------------------------------------------------

TEST_CASE("dilateMax grows the foreground by r px (square)", "[conv_viewsyn]")
{
	const uint32_t w = 9, h = 9;
	std::vector<float> d(w * h, 0.0f), tmp(w * h), o(w * h);
	d[4 * w + 4] = 1.0f;
	u_conv_viewsyn_ref_dilate_max(d.data(), w, h, 2, tmp.data(), o.data());
	float count = 0;
	for (float v : o) {
		count += v;
	}
	CHECK(count == 25.0f);
	CHECK(o[2 * w + 2] == 1.0f);
	CHECK(o[1 * w + 4] == 0.0f);
	u_conv_viewsyn_ref_dilate_max(d.data(), w, h, 0, tmp.data(), o.data());
	CHECK(o == d);
}

TEST_CASE("toDisparity: metric -> 1/z, passthrough otherwise", "[conv_viewsyn]")
{
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_METRIC, 2.0) == 0.5);
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_METRIC, 0.5) == 2.0);
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_METRIC, 0.0) == 0.0);
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_AFFINE_DISPARITY, 1.0) == 1.0);
}

TEST_CASE("viewEye inverts kooimaProjection (eye in window heights)", "[conv_viewsyn]")
{
	const V3 eyes[] = {v3(0, 0, 2.5), v3(0.13, -0.07, 1.8), v3(-0.4, 0.2, 4)};
	const double aspects[] = {1.0, 16.0 / 9.0, 0.75};
	for (const V3 &eye : eyes) {
		for (double A : aspects) {
			float P[16];
			u_conv_viewsyn_kooima_projection(eye, A, 0.05, 100.0, P);
			double a = 0;
			V3 e = u_conv_viewsyn_eye_from_projection(P, &a);
			NEAR(e.x, eye.x, 1e-5);
			NEAR(e.y, eye.y, 1e-5);
			NEAR(e.z, eye.z, 1e-5);
			NEAR(a, A, 1e-5);
		}
	}
}

TEST_CASE("qScale: nominal pair sees budget*gain of the width across n in [0,1]", "[conv_viewsyn]")
{
	const double A = 16.0 / 9.0, D0 = 2.5, budget = 0.025;
	double qs = u_conv_viewsyn_q_scale(budget, 1, A, D0, U_CONV_VIEWSYN_KAPPA);
	// Eyes at +-b/2, b = KAPPA*D0; screen-space shift = e.x*q (window heights); disparity = b*qs.
	double disparity_frac_of_width = (U_CONV_VIEWSYN_KAPPA * D0 * qs) / A;
	NEAR(disparity_frac_of_width, budget, 1e-9);
	NEAR(u_conv_viewsyn_q_scale(budget, 2, A, D0, U_CONV_VIEWSYN_KAPPA), 2 * qs, 1e-12); // gain is linear
}

TEST_CASE("panel matrices ARE plain Kooima frusta and viewEye reads them", "[conv_viewsyn]")
{
	for (const float *m : {kPanel[0], kPanel[1]}) {
		double a = 0;
		V3 e = u_conv_viewsyn_eye_from_projection(m, &a);
		float k[16];
		u_conv_viewsyn_kooima_projection(e, a, 0.05, 100.0, k);
		for (int i : {0, 5, 8, 9}) {
			NEAR(k[i], m[i], 1e-5);
		}
		NEAR(a, 16.0 / 9.0, 1e-5);
		CHECK(e.y > 0.4); // eye ABOVE the tile centre (+y up)
		CHECK(e.y < 0.5);
		NEAR(e.z, 5.477, 0.01); // eye distance, tile heights
	}
	V3 eyes[2];
	double a;
	panel_eyes(eyes, &a);
	CHECK(eyes[0].x < eyes[1].x); // view 0 is the left eye
	NEAR(eyes[1].x - eyes[0].x, 0.6118, 1e-3);
}

TEST_CASE("sourceCamera = eye centroid; lookAround blends x,y toward the anchor, never z", "[conv_viewsyn]")
{
	const V3 eyes[2] = {v3(-0.4, 0.44, 5.48), v3(0.2, 0.46, 5.47)};
	V3 c = u_conv_viewsyn_source_camera(eyes, 2, nullptr, 0.0);
	NEAR(c.x, -0.1, 1e-12);
	NEAR(c.y, 0.45, 1e-12);
	NEAR(c.z, 5.475, 1e-12);
	V3 far_anchor = v3(9, 9, 0);
	V3 c0 = u_conv_viewsyn_source_camera(eyes, 2, &far_anchor, 0.0); // lookAround 0 ignores the anchor
	CHECK(c0.x == c.x);
	CHECK(c0.y == c.y);
	CHECK(c0.z == c.z);
	V3 anchor = v3(0.1, 0.25, 0);
	V3 hh = u_conv_viewsyn_source_camera(eyes, 2, &anchor, 0.5);
	NEAR(hh.x, 0.0, 1e-12);
	NEAR(hh.y, 0.35, 1e-12);
	NEAR(hh.z, 5.475, 1e-12);
	V3 f = u_conv_viewsyn_source_camera(eyes, 2, &anchor, 1.0);
	NEAR(f.x, 0.1, 1e-12);
	NEAR(f.y, 0.25, 1e-12);
}

TEST_CASE("panel eyes, flat source ON the glass (q = 0): both eyes are the identity", "[conv_viewsyn]")
{
	V3 eyes[2];
	double A;
	panel_eyes(eyes, &A);
	V3 cam = u_conv_viewsyn_source_camera(eyes, 2, nullptr, 0.0);
	for (const V3 &e : eyes) {
		for (double v = 0; v <= 1.0001; v += 0.125) {
			for (double u = 0; u <= 1.0001; u += 0.125) {
				const double uv[2] = {u, v};
				double s[2];
				u_conv_viewsyn_src_uv(uv, 0.0, e, cam, A, s);
				NEAR(s[0], u, 1e-12);
				NEAR(s[1], v, 1e-12);
			}
		}
	}
}

TEST_CASE("an eye AT the source camera is the identity for every depth", "[conv_viewsyn]")
{
	const V3 cam = v3(-0.1, 0.45, 5.5);
	for (double q : {-0.08, -0.02, 0.03, 0.08}) {
		for (const auto &uv : {std::vector<double>{0.1, 0.9}, {0.5, 0.5}, {0.95, 0.05}}) {
			double s[2];
			u_conv_viewsyn_src_uv(uv.data(), q, cam, cam, 16.0 / 9.0, s);
			NEAR(s[0], uv[0], 1e-12);
			NEAR(s[1], uv[1], 1e-12);
		}
	}
}

TEST_CASE("panel eyes, real depth: near and far move OPPOSITE ways horizontally, not vertically", "[conv_viewsyn]")
{
	V3 eyes[2];
	double A;
	panel_eyes(eyes, &A);
	V3 cam = u_conv_viewsyn_source_camera(eyes, 2, nullptr, 0.0);
	double qs = u_conv_viewsyn_q_scale(0.025, 1, A, cam.z, U_CONV_VIEWSYN_KAPPA);
	const double conv = 0.6;
	const double q_near = qs * (1 - conv), q_far = qs * (0 - conv);
	for (const V3 &e : eyes) {
		double side = sgn(e.x - cam.x);
		// content displacement in px = -(source offset); source offset at uv (0.5, 0.5)
		auto move = [&](double q, double *dx, double *dy) {
			const double uv[2] = {0.5, 0.5};
			double s[2];
			u_conv_viewsyn_src_uv(uv, q, e, cam, A, s);
			*dx = -(s[0] - 0.5) * A * kPanelViewH;
			*dy = -(s[1] - 0.5) * kPanelViewH;
		};
		double ndx, ndy, fdx, fdy, cdx, cdy;
		move(q_near, &ndx, &ndy);
		move(q_far, &fdx, &fdy);
		move(0.0, &cdx, &cdy);
		CHECK(sgn(ndx) == -side); // near: crossed
		CHECK(sgn(fdx) == side);  // far: uncrossed
		CHECK(std::fabs(ndx) > 3);
		CHECK(std::fabs(fdx) > 3);
		NEAR(cdx, 0, 1e-9);
		NEAR(cdy, 0, 1e-9);
		CHECK(std::fabs(ndy) < 0.5); // only the eyes' own height difference (head roll)
		CHECK(std::fabs(fdy) < 0.5);
	}
}

TEST_CASE("REGRESSION: pre-fix source camera on the tile axis shears the frame by E.y*q", "[conv_viewsyn]")
{
	V3 eyes[2];
	double A;
	panel_eyes(eyes, &A);
	double D0 = u_conv_viewsyn_source_camera(eyes, 2, nullptr, 0.0).z;
	V3 on_axis = v3(0, 0, D0);
	double q_far = u_conv_viewsyn_q_scale(0.025, 1, A, D0, U_CONV_VIEWSYN_KAPPA) * -0.6;
	for (const V3 &e : eyes) {
		const double uv[2] = {0.5, 0.5};
		double s[2];
		u_conv_viewsyn_src_uv(uv, q_far, e, on_axis, A, s);
		double dy = (s[1] - 0.5) * kPanelViewH;                         // source offset, v up
		NEAR(std::fabs(dy), std::fabs(e.y * q_far) * kPanelViewH, 0.5); // shear = E.y*q
		CHECK(std::fabs(dy) > 15);
	}
}

TEST_CASE("erodeMin shrinks the foreground by r px; snapPack interleaves d/min/max", "[conv_viewsyn]")
{
	const uint32_t w = 9, h = 9;
	std::vector<float> d(w * h, 0.0f), tmp(w * h), tmp2(w * h), e(w * h), p(w * h * 4);
	for (uint32_t y = 2; y <= 6; y++) {
		for (uint32_t x = 2; x <= 6; x++) {
			d[y * w + x] = 1.0f; // 5x5 block
		}
	}
	u_conv_viewsyn_ref_erode_min(d.data(), w, h, 2, tmp.data(), e.data());
	float sum = 0;
	for (float v : e) {
		sum += v;
	}
	CHECK(sum == 1.0f); // only the centre survives
	CHECK(e[4 * w + 4] == 1.0f);
	u_conv_viewsyn_ref_snap_pack(d.data(), w, h, 1, tmp.data(), tmp2.data(), p.data());
	CHECK(p[4 * (2 * w + 2)] == 1.0f);     // r = d
	CHECK(p[4 * (2 * w + 2) + 1] == 0.0f); // corner eroded away
	CHECK(p[4 * (1 * w + 1) + 2] == 1.0f); // dilated outward
	CHECK(p[4 * (4 * w + 4) + 3] == 1.0f);
}

TEST_CASE("snapRadii scale with the depth width, radius capped at 12", "[conv_viewsyn]")
{
	uint32_t core, radius;
	u_conv_viewsyn_snap_radii(4, 8, 364, &core, &radius);
	CHECK(core == 4);
	CHECK(radius == 8);
	u_conv_viewsyn_snap_radii(4, 8, 518, &core, &radius);
	CHECK(core == 6);
	CHECK(radius == 11);
	u_conv_viewsyn_snap_radii(4, 20, 518, &core, &radius);
	CHECK(radius == 12);
}

// ---- runtime additions -----------------------------------------------------------------------

TEST_CASE("declared depth kinds decode to nearness (larger = nearer)", "[conv_viewsyn]")
{
	u_conv_viewsyn_depth_desc d = {U_CONV_VIEWSYN_DEPTH_METRIC, 0.0f, 0.0f, NAN};
	NEAR(u_conv_viewsyn_decode(&d, 3.0), 3.0, 0); // scale 0 is read as 1
	d.value_scale = 2.0f;
	d.value_shift = 1.0f;
	NEAR(u_conv_viewsyn_decode(&d, 3.0), 7.0, 0);
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_AFFINE, 2.0) <
	      u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_AFFINE, 1.0));
	CHECK(u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_METRIC, 2.0) <
	      u_conv_viewsyn_nearness(U_CONV_VIEWSYN_DEPTH_METRIC, 1.0));
	for (uint32_t k : {U_CONV_VIEWSYN_DEPTH_METRIC_INVERSE, U_CONV_VIEWSYN_DEPTH_AFFINE_INVERSE,
	                   U_CONV_VIEWSYN_DEPTH_AFFINE_DISPARITY, U_CONV_VIEWSYN_DEPTH_STEREO_DISPARITY_PX,
	                   U_CONV_VIEWSYN_DEPTH_UNKNOWN}) {
		CHECK(u_conv_viewsyn_nearness(k, 0.7) == 0.7);
	}
	// Normalise: metric 1 m .. 4 m -> nearness 0.25 .. 1; 2 m sits at 1/3; invalid reads far.
	u_conv_viewsyn_depth_desc m = {U_CONV_VIEWSYN_DEPTH_METRIC, 1.0f, 0.0f, 0.0f};
	NEAR(u_conv_viewsyn_normalise(&m, 0.25, 1.0, 2.0), 1.0 / 3.0, 1e-12);
	CHECK(u_conv_viewsyn_normalise(&m, 0.25, 1.0, 0.0) == 0.0);
}

TEST_CASE("resolve: viewpoints in metres = the eyes viewEye reads from their Kooima projections", "[conv_viewsyn]")
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = 0.32f;
	p.rect_height_m = 0.18f;
	p.view_count = 2;
	const float vps[2][3] = {{-0.0315f, 0.10f, 0.60f}, {0.0315f, 0.10f, 0.60f}};
	memcpy(p.viewpoints_m, vps, sizeof(vps));
	p.depth.kind = U_CONV_VIEWSYN_DEPTH_METRIC;
	p.range_near = 1.0f;  // metres: n = 1
	p.range_far = 4.0f;   // n = 0
	p.convergence = 2.0f; // decoded: 2 m on the glass
	u_conv_viewsyn_frame f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &f));
	NEAR(f.aspect, 0.32 / 0.18, 1e-6);
	for (uint32_t i = 0; i < 2; i++) {
		V3 e = v3(vps[i][0] / 0.18, vps[i][1] / 0.18, vps[i][2] / 0.18);
		float P[16];
		u_conv_viewsyn_kooima_projection(e, f.aspect, 0.05, 100.0, P);
		double a;
		V3 r = u_conv_viewsyn_eye_from_projection(P, &a);
		NEAR(f.views[i].eye.x, r.x, 1e-5);
		NEAR(f.views[i].eye.y, r.y, 1e-5);
		NEAR(f.views[i].eye.z, r.z, 1e-5);
		NEAR(f.views[i].q_scale, u_conv_viewsyn_q_scale(p.budget, 1, f.aspect, f.cam.z, U_CONV_VIEWSYN_KAPPA),
		     1e-12);
	}
	NEAR(f.cam.x, 0.0, 1e-7);
	NEAR(f.cam.y, 0.10 / 0.18, 1e-6);
	NEAR(f.near_lo, 0.25, 1e-7);
	NEAR(f.near_hi, 1.0, 1e-7);
	NEAR(f.conv_n, (0.5 - 0.25) / 0.75, 1e-7); // decoded convergence, mapped once, not re-normalised
	CHECK(f.rear_k == 1.0);                    // unlimited by default

	// Reversed range order is the same range; a flat range keeps a non-zero span.
	p.range_near = 4.0f;
	p.range_far = 1.0f;
	u_conv_viewsyn_frame g;
	REQUIRE(u_conv_viewsyn_resolve(&p, &g));
	CHECK(g.near_lo == f.near_lo);
	CHECK(g.near_hi == f.near_hi);
	p.range_near = p.range_far = 2.0f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &g));
	CHECK(g.near_hi > g.near_lo);

	// Invalid geometry is refused.
	p.rect_height_m = 0;
	CHECK_FALSE(u_conv_viewsyn_resolve(&p, &g));
	p.rect_height_m = 0.18f;
	p.view_count = 0;
	CHECK_FALSE(u_conv_viewsyn_resolve(&p, &g));
}

TEST_CASE("ADR-040 rear cap: the deepest content lands exactly on the cap, nearer content untouched", "[conv_viewsyn]")
{
	const double qs = 0.05, conv = 0.6, cam_z = 3.0;
	CHECK(u_conv_viewsyn_rear_k(qs, conv, cam_z, -1.0) == 1.0); // unlimited
	CHECK(u_conv_viewsyn_rear_k(qs, conv, cam_z, 10.0) == 1.0); // not binding
	CHECK(u_conv_viewsyn_rear_k(qs, conv, cam_z, 0.0) == 0.0);  // nothing behind the glass
	const double R = 0.02;                                      // window heights
	double k = u_conv_viewsyn_rear_k(qs, conv, cam_z, R);
	REQUIRE(k > 0.0);
	REQUIRE(k < 1.0);
	double q = qs * (0 - conv) * k;   // deepest q after the cap
	double z = q * cam_z / (1.0 + q); // window heights, negative = behind
	NEAR(z, -R, 1e-12);

	// Through resolve + q_at: front content is never scaled.
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = 0.32f;
	p.rect_height_m = 0.18f;
	p.view_count = 1;
	p.viewpoints_m[0][2] = 0.6f;
	p.rear_cap_m = 0.0036f; // 0.02 window heights
	p.edge_taper = 0;
	u_conv_viewsyn_frame f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &f));
	CHECK(f.rear_k < 1.0);
	auto near_fn = [](void *, double, double) { return 1.0; };
	auto far_fn = [](void *, double, double) { return 0.0; };
	const double uv[2] = {0.5, 0.5};
	NEAR(u_conv_viewsyn_q_at(&f, &f.views[0], near_fn, nullptr, uv), f.views[0].q_scale * (1 - f.conv_n), 1e-12);
	double qf = u_conv_viewsyn_q_at(&f, &f.views[0], far_fn, nullptr, uv);
	NEAR(qf * f.cam.z / (1 + qf), -0.02, 1e-6);
}

namespace {

//! A step edge: a near square (u = 1) over a far ground (u = 0), source uv v up.
struct step_scene
{
	double x0, x1, y0, y1;
};

double
step_sample(void *user, double u, double v)
{
	const step_scene *s = static_cast<const step_scene *>(user);
	return (u >= s->x0 && u <= s->x1 && v >= s->y0 && v <= s->y1) ? 1.0 : 0.0;
}

} // namespace

TEST_CASE("march: flat map at convergence is the identity; plateaus land on their own parallax", "[conv_viewsyn]")
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = 0.32f;
	p.rect_height_m = 0.18f;
	p.view_count = 2;
	const float vps[2][3] = {{-0.0315f, 0.0f, 0.6f}, {0.0315f, 0.0f, 0.6f}};
	memcpy(p.viewpoints_m, vps, sizeof(vps));
	p.convergence = 0.5f; // affine disparity, range [0, 1]
	p.edge_taper = 0;
	u_conv_viewsyn_frame f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &f));

	auto conv_fn = [](void *, double, double) { return 0.5; };
	for (uint32_t i = 0; i < 2; i++) {
		const double uv[2] = {0.3, 0.7};
		double s[2];
		double q = u_conv_viewsyn_march(&f, &f.views[i], conv_fn, nullptr, uv, s);
		NEAR(q, 0.0, 1e-3 * f.views[i].q_scale);
		NEAR(s[0], uv[0], 1e-4);
		NEAR(s[1], uv[1], 1e-4);
	}

	step_scene sc = {0.35, 0.65, 0.25, 0.75};
	const double q_near = f.views[0].q_scale * (1 - f.conv_n);
	const double q_far = f.views[0].q_scale * (0 - f.conv_n);
	double shift_near[2], shift_far[2];
	for (uint32_t i = 0; i < 2; i++) {
		// Plateau centre and far ground: the march must find each one's own q,
		// to within one bisection interval.
		const double tol = (q_near - q_far) / f.steps / 32.0 + 1e-9;
		const double c[2] = {0.5, 0.5};
		const double g[2] = {0.1, 0.5};
		double s[2];
		double qc = u_conv_viewsyn_march(&f, &f.views[i], step_sample, &sc, c, s);
		NEAR(qc, q_near, tol);
		shift_near[i] = (c[0] - s[0]) * f.aspect; // content displacement, window heights
		double qg = u_conv_viewsyn_march(&f, &f.views[i], step_sample, &sc, g, s);
		NEAR(qg, q_far, tol);
		shift_far[i] = (g[0] - s[0]) * f.aspect;
	}
	// Near content crossed, far uncrossed; each view mirrors the other.
	CHECK(shift_near[1] < 0);
	CHECK(shift_near[0] > 0);
	CHECK(shift_far[1] > 0);
	CHECK(shift_far[0] < 0);
	NEAR(shift_near[0], -shift_near[1], 1e-4);
	// Disparity across the full range = budget of the width for the nominal pair
	// (this rig IS the nominal pair: 63 mm at 0.6 m).
	double disparity = (shift_near[0] - shift_near[1]) - (shift_far[0] - shift_far[1]);
	NEAR(disparity / f.aspect, p.budget, 2e-4);
}

TEST_CASE("resolve: N views share one source camera at their centroid", "[conv_viewsyn]")
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = 0.32f;
	p.rect_height_m = 0.18f;
	p.view_count = 4;
	const float xs[4] = {-0.063f, -0.021f, 0.021f, 0.063f};
	for (uint32_t i = 0; i < 4; i++) {
		p.viewpoints_m[i][0] = xs[i];
		p.viewpoints_m[i][1] = 0.02f;
		p.viewpoints_m[i][2] = 0.6f;
	}
	u_conv_viewsyn_frame f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &f));
	INFO("cam " << f.cam.x << " " << f.cam.y << " " << f.cam.z);
	NEAR(f.cam.x, 0.0, 1e-7);
	NEAR(f.cam.y, 0.02 / 0.18, 1e-6);
	NEAR(f.cam.z, 0.6 / 0.18, 1e-6);
	for (uint32_t i = 0; i < 4; i++) {
		NEAR(f.views[i].eye.x, xs[i] / 0.18, 1e-6);
		CHECK(std::isfinite(f.views[i].q_scale));
	}
}

TEST_CASE("sourceCamera: centroid for every view count 1..8", "[conv_viewsyn]")
{
	for (uint32_t n = 1; n <= U_CONV_VIEWSYN_MAX_VIEWS; n++) {
		V3 eyes[U_CONV_VIEWSYN_MAX_VIEWS];
		double sx = 0, sy = 0, sz = 0;
		for (uint32_t i = 0; i < n; i++) {
			eyes[i] = v3(0.1 * i - 0.3, 0.05 * i, 3.0 + 0.01 * i);
			sx += eyes[i].x;
			sy += eyes[i].y;
			sz += eyes[i].z;
		}
		V3 c = u_conv_viewsyn_source_camera(eyes, n, nullptr, 0.0);
		INFO("n " << n << " cam " << c.x << " " << c.y << " " << c.z);
		NEAR(c.x, sx / n, 1e-12);
		NEAR(c.y, sy / n, 1e-12);
		NEAR(c.z, sz / n, 1e-12);
	}
}
