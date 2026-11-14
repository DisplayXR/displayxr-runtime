// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Legacy automatic 2D + automatic re-convergence (util/u_stereo_auto.h):
 *         the flat detector, the disparity estimate, the comfort clamp, the
 *         hysteresis and the temporal policy, on synthetic eye pairs; plus the
 *         profile keys that switch them on (util/u_camera_profile.h).
 */

#include "catch_amalgamated.hpp"

#include "util/u_stereo_auto.h"
#include "util/u_camera_profile.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t W = 512; //!< eye width, px (2 px per grid cell)
constexpr uint32_t H = 288;
constexpr uint64_t PROBE = 100ull * 1000ull * 1000ull;

//! Deterministic value noise: textured at a few px, smooth enough to match.
float
tex(uint32_t seed, double u, double v)
{
	auto hash = [seed](int32_t x, int32_t y) {
		uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
	};
	const double sx = u / 6.0, sy = v / 10.0;
	const int32_t x0 = (int32_t)std::floor(sx), y0 = (int32_t)std::floor(sy);
	const float tx = (float)(sx - x0), ty = (float)(sy - y0);
	const float a = hash(x0, y0), b = hash(x0 + 1, y0), c = hash(x0, y0 + 1), d = hash(x0 + 1, y0 + 1);
	const float top = a + (b - a) * tx, bot = c + (d - c) * tx;
	return 40.0f + 170.0f * (top + (bot - top) * ty);
}

struct Scene
{
	uint32_t seed = 1;
	double bg_disp_px = 0.0; //!< background disparity, px (positive = behind)
	bool subject = false;
	double subj_disp_px = 0.0;     //!< subject disparity, px
	double sx0 = 0.30, sx1 = 0.70; //!< subject rect, fraction of the eye
	double sy0 = 0.25, sy1 = 0.80;
};

//! One eye's luma. World column u is seen at x = u (left) or x = u + d (right).
std::vector<float>
render(const Scene &s, bool right_eye)
{
	std::vector<float> img(W * H);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			float v;
			bool in_subject = false;
			if (s.subject) {
				const double u = right_eye ? x - s.subj_disp_px : x;
				in_subject = u >= s.sx0 * W && u < s.sx1 * W && y >= s.sy0 * H && y < s.sy1 * H;
				if (in_subject) {
					v = tex(s.seed + 77, u, y);
				}
			}
			if (!in_subject) {
				const double u = right_eye ? x - s.bg_disp_px : x;
				v = tex(s.seed, u, y);
			}
			img[y * W + x] = v;
		}
	}
	return img;
}

void
grids(const Scene &s, std::vector<float> &l, std::vector<float> &r)
{
	l.assign(U_STEREO_AUTO_CELLS, 0.0f);
	r.assign(U_STEREO_AUTO_CELLS, 0.0f);
	const auto li = render(s, false), ri = render(s, true);
	u_stereo_auto_grid_from_luma(li.data(), W, H, 0, W, l.data());
	u_stereo_auto_grid_from_luma(ri.data(), W, H, 0, W, r.data());
}

void
add_noise(std::vector<float> &g, uint32_t seed, float amp)
{
	uint32_t st = seed * 2654435761u + 1u;
	for (float &v : g) {
		st = st * 1664525u + 1013904223u;
		v += amp * (((float)(st >> 8) / 16777216.0f) * 2.0f - 1.0f);
	}
}

u_legacy_stereo_auto
cfg_on()
{
	u_legacy_stereo_auto c = U_LEGACY_STEREO_AUTO_DEFAULT;
	c.auto_2d = true;
	c.auto_convergence = true;
	c.max_shift_fraction = 0.05f;
	c.max_behind_fraction = 0.05f;
	c.max_front_fraction = 0.05f;
	return c;
}

} // namespace

TEST_CASE("grid: area average over a side-by-side plane picks the right eye's columns")
{
	std::vector<float> plane(2 * 64 * 24);
	for (uint32_t y = 0; y < 24; y++) {
		for (uint32_t x = 0; x < 128; x++) {
			plane[y * 128 + x] = x < 64 ? 10.0f : 200.0f;
		}
	}
	std::vector<float> g(U_STEREO_AUTO_CELLS);
	// An eye narrower than the grid still fills every cell.
	u_stereo_auto_grid_from_luma(plane.data(), 128, 24, 64, 64, g.data());
	for (float v : g) {
		REQUIRE(v == Catch::Approx(200.0f));
	}
	u_stereo_auto_grid_from_luma(plane.data(), 128, 24, 0, 64, g.data());
	REQUIRE(g[0] == Catch::Approx(10.0f));
}

TEST_CASE("flat: identical eyes with sampling noise are one flat picture")
{
	Scene s;
	std::vector<float> l, r;
	grids(s, l, r);
	add_noise(r, 3, 1.5f);
	u_stereo_flat_params p;
	u_stereo_flat_params_default(&p);
	u_stereo_flat_measurement m;
	u_stereo_flat_measure(l.data(), r.data(), &p, &m);
	REQUIRE(m.flat);
	REQUIRE(m.unshifted < 3.0);
}

TEST_CASE("flat: a picture drawn at one depth (one horizontal shift) is flat too")
{
	Scene s;
	s.bg_disp_px = 9.0; // 4.5 cells behind the screen
	std::vector<float> l, r;
	grids(s, l, r);
	u_stereo_flat_params p;
	u_stereo_flat_params_default(&p);
	u_stereo_flat_measurement m;
	u_stereo_flat_measure(l.data(), r.data(), &p, &m);
	REQUIRE(m.unshifted > 3.0);
	REQUIRE(m.flat);
	// The right eye shows it 4.5 cells to the right: sampled at x - shift, shift = -4.5.
	REQUIRE(m.best_shift == Catch::Approx(-4.5).margin(0.25));
}

TEST_CASE("flat: a scene with two depths is not flat, and its subject is measured")
{
	Scene s;
	s.bg_disp_px = 10.0; // behind
	s.subject = true;
	s.subj_disp_px = -8.0; // in front
	std::vector<float> l, r;
	grids(s, l, r);
	u_stereo_flat_params p;
	u_stereo_flat_params_default(&p);
	u_stereo_flat_measurement m;
	u_stereo_flat_measure(l.data(), r.data(), &p, &m);
	REQUIRE_FALSE(m.flat);

	u_stereo_disparity_estimate e;
	REQUIRE(u_stereo_auto_estimate(l.data(), r.data(), 0.2, &e));
	REQUIRE(e.blocks >= 40);
	const float subj = -8.0f / W, bg = 10.0f / W;
	REQUIRE(e.near_d == Catch::Approx(subj).margin(1.0 / 256));
	REQUIRE(e.far_d == Catch::Approx(bg).margin(1.0 / 256));
	// The centre-weighted screen plane sits on (or just behind) the subject in
	// the middle of the frame, never on the background.
	REQUIRE(e.screen < subj + 0.4f * (bg - subj));
	REQUIRE(e.screen > subj - 1.0f / 256);
}

TEST_CASE("estimate: uncorrelated eyes give no confident disparity")
{
	Scene a, b;
	b.seed = 99;
	std::vector<float> l, r, l2, r2;
	grids(a, l, r);
	grids(b, l2, r2);
	u_stereo_disparity_estimate e;
	(void)u_stereo_auto_estimate(l.data(), r2.data(), 0.2, &e);
	REQUIRE(e.blocks < U_STEREO_AUTO_BLOCKS / 4);
}

TEST_CASE("target shift: screen plane to zero, then the comfort bounds and the cap")
{
	u_stereo_disparity_estimate e{};
	e.valid = true;
	e.screen = -0.01f;
	e.near_d = -0.015f;
	e.far_d = 0.01f;
	u_legacy_stereo_auto c = cfg_on();
	c.max_behind_fraction = 0.03f;
	c.max_front_fraction = 0.02f;
	c.max_shift_fraction = 0.03f;
	// Unconstrained: +0.01 puts the screen plane at zero (far ends at 0.02).
	REQUIRE(u_stereo_auto_target_shift(&e, &c) == Catch::Approx(0.01f));
	// The far content would end up 0.035 behind: clamped to max_behind - far = 0.005.
	e.far_d = 0.025f;
	REQUIRE(u_stereo_auto_target_shift(&e, &c) == Catch::Approx(0.005f));
	// More depth than the budget: the middle of the two violated bounds
	// (lo = -0.02 + 0.04 = 0.02, hi = 0.03 - 0.04 = -0.01).
	e.near_d = -0.04f;
	e.far_d = 0.04f;
	REQUIRE(u_stereo_auto_target_shift(&e, &c) == Catch::Approx(0.005f));
	// The cap.
	e.near_d = -0.1f;
	e.far_d = -0.06f;
	e.screen = -0.08f;
	REQUIRE(u_stereo_auto_target_shift(&e, &c) == Catch::Approx(0.03f));
	// Invalid estimate: no shift.
	e.valid = false;
	REQUIRE(u_stereo_auto_target_shift(&e, &c) == 0.0f);
}

TEST_CASE("hysteresis: 2D after five flat probes, 3D on the second probe with depth")
{
	Scene flat_s;
	Scene deep_s;
	deep_s.bg_disp_px = 10.0;
	deep_s.subject = true;
	deep_s.subj_disp_px = -8.0;
	std::vector<float> fl, fr, dl, dr;
	grids(flat_s, fl, fr);
	grids(deep_s, dl, dr);
	const u_legacy_stereo_auto c = cfg_on();
	u_stereo_auto a;
	u_stereo_auto_init(&a, &c);
	u_stereo_auto_result res;
	uint64_t t = PROBE;
	for (int i = 0; i < 4; i++) {
		u_stereo_auto_probe(&a, fl.data(), fr.data(), t += PROBE, &res);
		REQUIRE_FALSE(res.want_2d);
	}
	u_stereo_auto_probe(&a, fl.data(), fr.data(), t += PROBE, &res);
	REQUIRE(res.want_2d);
	// One probe of depth (a single frame of a cut) keeps 2D ...
	u_stereo_auto_probe(&a, dl.data(), dr.data(), t += PROBE, &res);
	REQUIRE(res.want_2d);
	// ... a flat probe in between restarts the count ...
	u_stereo_auto_probe(&a, fl.data(), fr.data(), t += PROBE, &res);
	REQUIRE(res.want_2d);
	u_stereo_auto_probe(&a, dl.data(), dr.data(), t += PROBE, &res);
	REQUIRE(res.want_2d);
	// ... and the second consecutive one leaves.
	u_stereo_auto_probe(&a, dl.data(), dr.data(), t += PROBE, &res);
	REQUIRE_FALSE(res.want_2d);
	// Leaving 2D snaps the shift straight onto the scene's target.
	REQUIRE(res.shift == Catch::Approx(res.target_shift));
}

TEST_CASE("noise: independent eyes never enter 2D")
{
	Scene a_s, b_s;
	b_s.seed = 55;
	std::vector<float> l, r, l2, r2;
	grids(a_s, l, r);
	grids(b_s, l2, r2);
	const u_legacy_stereo_auto c = cfg_on();
	u_stereo_auto a;
	u_stereo_auto_init(&a, &c);
	u_stereo_auto_result res;
	uint64_t t = 0;
	for (int i = 0; i < 20; i++) {
		std::vector<float> rn = r2;
		add_noise(rn, (uint32_t)i + 1, 4.0f);
		u_stereo_auto_probe(&a, l.data(), rn.data(), t += PROBE, &res);
		REQUIRE_FALSE(res.want_2d);
		REQUIRE_FALSE(res.flat.flat);
	}
}

TEST_CASE("auto-2D off: flat pictures never ask for 2D")
{
	Scene s;
	std::vector<float> l, r;
	grids(s, l, r);
	u_legacy_stereo_auto c = cfg_on();
	c.auto_2d = false;
	u_stereo_auto a;
	u_stereo_auto_init(&a, &c);
	u_stereo_auto_result res;
	uint64_t t = 0;
	for (int i = 0; i < 10; i++) {
		u_stereo_auto_probe(&a, l.data(), r.data(), t += PROBE, &res);
		REQUIRE(res.flat.flat);
		REQUIRE_FALSE(res.want_2d);
	}
}

TEST_CASE("convergence: the first estimate snaps, a small change is followed slowly, a cut snaps")
{
	Scene s1;
	s1.bg_disp_px = 10.0;
	s1.subject = true;
	s1.subj_disp_px = -8.0;
	Scene s1b = s1;
	s1b.subj_disp_px = -6.0; // the subject drifts a little: same shot
	Scene s2;
	s2.seed = 9; // a different scene: a cut
	s2.bg_disp_px = 12.0;
	s2.subject = true;
	s2.subj_disp_px = 4.0;
	std::vector<float> l1, r1, l1b, r1b, l2, r2;
	grids(s1, l1, r1);
	grids(s1b, l1b, r1b);
	grids(s2, l2, r2);
	u_legacy_stereo_auto c = cfg_on();
	c.auto_2d = false;
	u_stereo_auto a;
	u_stereo_auto_init(&a, &c);
	u_stereo_auto_result res;
	uint64_t t = 0;

	u_stereo_auto_probe(&a, l1.data(), r1.data(), t += PROBE, &res);
	REQUIRE(res.est.valid);
	REQUIRE_FALSE(res.cut);
	REQUIRE(res.shift == Catch::Approx(res.target_shift));
	// Roughly the subject to the screen: +8 px of 512.
	REQUIRE(res.shift > 0.6f * 8.0f / W);
	REQUIRE(res.shift < 1.6f * 8.0f / W);
	const float first = res.shift;

	u_stereo_auto_probe(&a, l1b.data(), r1b.data(), t += PROBE, &res);
	REQUIRE_FALSE(res.cut);
	REQUIRE(res.target_shift < first);
	// One 100 ms step of a 1 s follower: about a tenth of the way.
	REQUIRE(res.shift < first);
	REQUIRE(res.shift > res.target_shift);

	u_stereo_auto_probe(&a, l2.data(), r2.data(), t += PROBE, &res);
	REQUIRE(res.cut);
	REQUIRE(res.shift == Catch::Approx(res.target_shift));
	REQUIRE(res.shift < 0.0f); // the new subject sat behind the screen: pulled forward
	REQUIRE(a.cuts == 1);
}

TEST_CASE("convergence: a flat picture holds the shift")
{
	Scene deep;
	deep.bg_disp_px = 10.0;
	deep.subject = true;
	deep.subj_disp_px = -8.0;
	Scene flat_s;
	std::vector<float> dl, dr, fl, fr;
	grids(deep, dl, dr);
	grids(flat_s, fl, fr);
	u_legacy_stereo_auto c = cfg_on();
	c.auto_2d = false;
	u_stereo_auto a;
	u_stereo_auto_init(&a, &c);
	u_stereo_auto_result res;
	u_stereo_auto_probe(&a, dl.data(), dr.data(), PROBE, &res);
	const float held = res.shift;
	REQUIRE(held != 0.0f);
	u_stereo_auto_probe(&a, fl.data(), fr.data(), 2 * PROBE, &res);
	REQUIRE_FALSE(res.estimated);
	REQUIRE(res.shift == held);
}

TEST_CASE("profile: the legacy picture-policy keys")
{
	u_camera_profile p;
	char err[192];
	REQUIRE(u_camera_profile_parse(R"({"auto2d": true, "autoConvergence": 1, "maxShiftFraction": 0.04,
	                                   "maxBehindFraction": 0.025, "maxFrontFraction": 0.015})",
	                               1.0f, &p, err, sizeof(err)));
	REQUIRE(err[0] == '\0'); // all known: no skipped-key notice
	REQUIRE(p.stereo.auto_2d);
	REQUIRE(p.stereo.auto_convergence);
	REQUIRE(p.stereo.max_shift_fraction == Catch::Approx(0.04f));
	REQUIRE(p.stereo.max_behind_fraction == Catch::Approx(0.025f));
	REQUIRE(p.stereo.max_front_fraction == Catch::Approx(0.015f));
	// The camera tuning is untouched by them.
	REQUIRE(p.ipd_factor == 1.0f);

	REQUIRE(u_camera_profile_parse(R"({"ipdFactor": 0.5})", 1.0f, &p, err, sizeof(err)));
	REQUIRE_FALSE(p.stereo.auto_2d);
	REQUIRE_FALSE(p.stereo.auto_convergence);
	REQUIRE(p.stereo.max_shift_fraction == Catch::Approx(0.03f));

	REQUIRE_FALSE(u_camera_profile_parse(R"({"auto2d": "yes"})", 1.0f, &p, err, sizeof(err)));
	REQUIRE_FALSE(u_camera_profile_parse(R"({"auto2d": 2})", 1.0f, &p, err, sizeof(err)));
	REQUIRE_FALSE(u_camera_profile_parse(R"({"maxShiftFraction": 0.5})", 1.0f, &p, err, sizeof(err)));
	REQUIRE_FALSE(u_camera_profile_parse(R"({"auto2d": true, "auto2d": false})", 1.0f, &p, err, sizeof(err)));
}

TEST_CASE("profile: env flag spellings")
{
	REQUIRE(u_legacy_stereo_auto_parse_flag("1") == 1);
	REQUIRE(u_legacy_stereo_auto_parse_flag("on") == 1);
	REQUIRE(u_legacy_stereo_auto_parse_flag("true") == 1);
	REQUIRE(u_legacy_stereo_auto_parse_flag("0") == 0);
	REQUIRE(u_legacy_stereo_auto_parse_flag("off") == 0);
	REQUIRE(u_legacy_stereo_auto_parse_flag("") == -1);
	REQUIRE(u_legacy_stereo_auto_parse_flag(nullptr) == -1);
	REQUIRE(u_legacy_stereo_auto_parse_flag("maybe") == -1);
}
