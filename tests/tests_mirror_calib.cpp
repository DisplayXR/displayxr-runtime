// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Mirror-alignment camera-pose calibration solver (math/m_mirror_calib.h).
 *
 * Synthetic: a hidden camera pose, alignments at known dots (the eye point on
 * the dot's normal at some head distance), the camera-space direction and an
 * iris-style distance estimate it would measure, plus noise; the solver must
 * recover the pose, refuse degenerate input, and say what it cannot observe.
 */

#include "catch_amalgamated.hpp"

#include "math/m_mirror_calib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Pose
{
	double t[3] = {0, 0, 0};
	double r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // camera -> display, row-major
};

void
mul3(const double a[9], const double b[9], double o[9])
{
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
		}
	}
}

//! R = Rz(roll) * Ry(yaw) * Rx(pitch), degrees.
Pose
make_pose(double tx, double ty, double tz, double pitch_deg, double yaw_deg = 0, double roll_deg = 0)
{
	Pose p;
	p.t[0] = tx;
	p.t[1] = ty;
	p.t[2] = tz;
	const double a = pitch_deg * kDeg, b = yaw_deg * kDeg, c = roll_deg * kDeg;
	const double rx[9] = {1, 0, 0, 0, cos(a), -sin(a), 0, sin(a), cos(a)};
	const double ry[9] = {cos(b), 0, sin(b), 0, 1, 0, -sin(b), 0, cos(b)};
	const double rz[9] = {cos(c), -sin(c), 0, sin(c), cos(c), 0, 0, 0, 1};
	double tmp[9];
	mul3(ry, rx, tmp);
	mul3(rz, tmp, p.r);
	return p;
}

struct Noise
{
	double dir_deg = 0.0;    //!< angular RMS of the direction noise
	double align_m = 0.0;    //!< per-axis sigma of the eye point's offset from the dot normal
	double depth_frac = 0.0; //!< sigma of the iris-depth relative error
	double iris_scale = 1.0; //!< true distance / estimated distance (the per-user bias)
};

//! One alignment at dot (dx, dy) with the eye point at distance z from the glass.
m_mirror_calib_sample
make_sample(const Pose &p, double dx, double dy, double z, const Noise &n, std::mt19937 &rng)
{
	std::normal_distribution<double> g(0.0, 1.0);
	const double e[3] = {dx + n.align_m * g(rng), dy + n.align_m * g(rng), z};
	const double d[3] = {e[0] - p.t[0], e[1] - p.t[1], e[2] - p.t[2]};
	// camera = R^T * d
	double u[3] = {p.r[0] * d[0] + p.r[3] * d[1] + p.r[6] * d[2], p.r[1] * d[0] + p.r[4] * d[1] + p.r[7] * d[2],
	               p.r[2] * d[0] + p.r[5] * d[1] + p.r[8] * d[2]};
	const double dist = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	for (double &x : u) {
		x /= dist;
	}
	if (n.dir_deg > 0.0) {
		// Two tangent-axis angles, each sigma = rms / sqrt(2).
		const double s = n.dir_deg * kDeg / sqrt(2.0);
		double a[3] = {0, 0, 0};
		// any vector not parallel to u
		const double h[3] = {fabs(u[0]) < 0.9 ? 1.0 : 0.0, fabs(u[0]) < 0.9 ? 0.0 : 1.0, 0.0};
		double t1[3] = {u[1] * h[2] - u[2] * h[1], u[2] * h[0] - u[0] * h[2], u[0] * h[1] - u[1] * h[0]};
		const double l1 = sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
		for (double &x : t1) {
			x /= l1;
		}
		const double t2[3] = {u[1] * t1[2] - u[2] * t1[1], u[2] * t1[0] - u[0] * t1[2],
		                      u[0] * t1[1] - u[1] * t1[0]};
		const double g1 = s * g(rng), g2 = s * g(rng);
		for (int i = 0; i < 3; i++) {
			a[i] = u[i] + g1 * t1[i] + g2 * t2[i];
		}
		const double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
		for (int i = 0; i < 3; i++) {
			u[i] = a[i] / la;
		}
	}
	m_mirror_calib_sample out = {};
	out.dot_m[0] = dx;
	out.dot_m[1] = dy;
	for (int i = 0; i < 3; i++) {
		out.dir[i] = u[i];
	}
	out.est_distance_m = dist / n.iris_scale * (1.0 + n.depth_frac * g(rng));
	return out;
}

//! The procedure's five dots on a 0.60 x 0.34 m panel at 30 % inset (centre, left, right, top, bottom).
std::vector<std::pair<double, double>>
five_dots()
{
	const double w = 0.60, h = 0.34;
	return {{0, 0}, {-0.2 * w, 0}, {0.2 * w, 0}, {0, 0.2 * h}, {0, -0.2 * h}};
}

double
t_err_xy(const m_mirror_calib_result &r, const Pose &p)
{
	return hypot(r.t_m[0] - p.t[0], r.t_m[1] - p.t[1]);
}

} // namespace

TEST_CASE("mirror calib: mode (a) noise-free is exact", "[mirror_calib]")
{
	std::mt19937 rng(1);
	const Pose p = make_pose(0.03, 0.185, 0.0, 0.0);
	std::vector<m_mirror_calib_sample> s;
	int i = 0;
	for (auto d : five_dots()) {
		s.push_back(make_sample(p, d.first, d.second, 0.5 + 0.05 * i++, Noise{}, rng));
	}
	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	m_mirror_calib_result r;
	REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
	CHECK(r.status == M_MIRROR_CALIB_OK);
	CHECK(t_err_xy(r, p) < 1e-9);
	CHECK(r.rms_m < 1e-9);
	CHECK(r.params == 2);
	CHECK(r.rank == 2);
	CHECK_FALSE(r.ill_conditioned);
	CHECK(r.q[3] == Catch::Approx(1.0));
	CHECK(r.iris_scale == Catch::Approx(1.0).epsilon(1e-9));
	for (int k = 0; k < (int)s.size(); k++) {
		CHECK(r.distance_m[k] > 0.4);
	}
}

namespace {

struct TrialStats
{
	double median_mm, rms_mm, p95_mm, max_mm, mean_iris;
};

//! Mode (a) over random plausible bezel poses, @p rounds x the five dots.
TrialStats
run_mode_a_trials(int trials, int rounds, unsigned seed)
{
	// 0.2 deg RMS direction noise, 2 mm RMS alignment noise, 7 % iris depth noise, 1.06 iris bias.
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> ux(-0.15, 0.15), uy(0.17, 0.22), uz(0.45, 0.80);
	const Noise n{0.2, 0.002 / sqrt(2.0), 0.07, 1.06};
	std::vector<double> errs;
	double iris = 0.0;
	for (int t = 0; t < trials; t++) {
		const Pose p = make_pose(ux(rng), uy(rng), 0.0, 0.0);
		std::vector<m_mirror_calib_sample> s;
		for (int k = 0; k < rounds; k++) {
			for (auto d : five_dots()) {
				s.push_back(make_sample(p, d.first, d.second, uz(rng), n, rng));
			}
		}
		m_mirror_calib_result r;
		REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), nullptr, &r));
		CHECK(r.rms_m < 0.008);
		errs.push_back(t_err_xy(r, p));
		iris += r.iris_scale;
	}
	double ss = 0.0;
	for (double e : errs) {
		ss += e * e;
	}
	std::sort(errs.begin(), errs.end());
	return {errs[trials / 2] * 1e3, sqrt(ss / trials) * 1e3, errs[(int)(trials * 0.95)] * 1e3, errs.back() * 1e3,
	        iris / trials};
}

} // namespace

TEST_CASE("mirror calib: mode (a) recovers T within 3 mm from 5 noisy samples", "[mirror_calib]")
{
	// The per-trial error is GEOMETRY-limited, not solver-limited: with a
	// top-bezel camera the centre / top / bottom dots all lie on (nearly) one
	// line through the camera, so T_y comes from where the left and right rays
	// cross the centre line (~32 deg apart at 30 % inset). Each ray carries
	// ~2.2 mm of perpendicular noise at the camera from 0.2 deg alone (s * dtheta
	// at 0.6 m) plus the 2 mm alignment noise, so a single 5-sample calibration
	// lands within 3 mm typically (median), not always (p95 ~6 mm). The live
	// check + arrow-key nudge of the procedure is what closes the tail.
	const TrialStats a = run_mode_a_trials(200, 1, 20261023);
	std::printf(
	    "[mirror_calib] mode (a), 200 random poses x 5 samples: |dT| median %.2f mm, rms %.2f mm, p95 %.2f "
	    "mm, max %.2f mm; mean iris scale %.4f (true 1.06)\n",
	    a.median_mm, a.rms_mm, a.p95_mm, a.max_mm, a.mean_iris);
	CHECK(a.median_mm < 3.0);
	CHECK(a.mean_iris == Catch::Approx(1.06).margin(0.02));

	const TrialStats b = run_mode_a_trials(200, 2, 20261024);
	std::printf(
	    "[mirror_calib] mode (a), 200 random poses x 10 samples: |dT| median %.2f mm, rms %.2f mm, p95 %.2f "
	    "mm, max %.2f mm\n",
	    b.median_mm, b.rms_mm, b.p95_mm, b.max_mm);
	CHECK(b.median_mm < 2.5);
	CHECK(b.rms_mm < 3.0);
}

TEST_CASE("mirror calib: all samples at one dot is refused (scale ambiguity)", "[mirror_calib]")
{
	std::mt19937 rng(2);
	const Pose p = make_pose(0.0, 0.18, 0.0, 0.0);
	std::vector<m_mirror_calib_sample> s;
	for (double z : {0.45, 0.55, 0.65, 0.75, 0.85}) {
		s.push_back(make_sample(p, 0.05, -0.02, z, Noise{}, rng));
	}
	for (auto mode : {M_MIRROR_CALIB_MODE_TRANSLATION, M_MIRROR_CALIB_MODE_PITCH}) {
		m_mirror_calib_options opt;
		m_mirror_calib_options_init(&opt);
		opt.mode = mode;
		m_mirror_calib_result r;
		CHECK_FALSE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
		CHECK(r.status == M_MIRROR_CALIB_ERROR_DEGENERATE);
		CHECK(std::string(r.reason).find("one dot") != std::string::npos);
		CHECK(std::string(m_mirror_calib_status_str(r.status)) == "degenerate");
	}
}

TEST_CASE("mirror calib: too few samples / dots per mode", "[mirror_calib]")
{
	std::mt19937 rng(3);
	const Pose p = make_pose(0.0, 0.18, 0.0, 0.0);
	std::vector<m_mirror_calib_sample> s;
	s.push_back(make_sample(p, 0.0, 0.0, 0.6, Noise{}, rng));
	m_mirror_calib_result r;
	CHECK_FALSE(m_mirror_calib_solve(s.data(), 1, nullptr, &r));
	CHECK(r.status == M_MIRROR_CALIB_ERROR_TOO_FEW_SAMPLES);

	// Mode (c) needs 7.
	for (auto d : five_dots()) {
		s.push_back(make_sample(p, d.first, d.second, 0.7, Noise{}, rng));
	}
	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	opt.mode = M_MIRROR_CALIB_MODE_FULL;
	CHECK_FALSE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
	CHECK(r.status == M_MIRROR_CALIB_ERROR_TOO_FEW_SAMPLES);

	// A zero direction is invalid input.
	s[0].dir[0] = s[0].dir[1] = s[0].dir[2] = 0.0;
	CHECK_FALSE(m_mirror_calib_solve(s.data(), (int)s.size(), nullptr, &r));
	CHECK(r.status == M_MIRROR_CALIB_ERROR_INVALID_INPUT);
}

TEST_CASE("mirror calib: T_z is not observable (it never enters the x, y equations)", "[mirror_calib]")
{
	// The truth sits 25 mm in front of the glass; noise-free data still fits
	// EXACTLY with T_z = the assumed value, in every mode: the data cannot
	// tell them apart, so the solver reports the assumption.
	std::mt19937 rng(4);
	const Pose p = make_pose(-0.04, 0.19, 0.025, 0.0);
	std::vector<m_mirror_calib_sample> s;
	int i = 0;
	for (auto d : five_dots()) {
		s.push_back(make_sample(p, d.first, d.second, 0.5 + 0.06 * i++, Noise{}, rng));
	}
	for (double assumed : {0.0, 0.01}) {
		m_mirror_calib_options opt;
		m_mirror_calib_options_init(&opt);
		opt.assumed_tz_m = assumed;
		m_mirror_calib_result r;
		REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
		CHECK(r.t_m[2] == assumed);
		CHECK(t_err_xy(r, p) < 1e-9);
		CHECK(r.rms_m < 1e-9);
	}
}

TEST_CASE("mirror calib: mode (b) recovers pitch", "[mirror_calib]")
{
	const Noise none{};
	const Noise noisy{0.2, 0.002, 0.07, 1.0};
	for (double pitch : {-8.0, 6.0, 15.0}) {
		std::mt19937 rng((unsigned)(100 + pitch));
		const Pose p = make_pose(0.02, 0.20, 0.0, pitch);
		for (const Noise *n : {&none, &noisy}) {
			std::vector<m_mirror_calib_sample> s;
			// Two head distances (what conditions pitch).
			for (double z : {0.45, 0.80}) {
				for (auto d : five_dots()) {
					s.push_back(make_sample(p, d.first, d.second, z, *n, rng));
				}
			}
			m_mirror_calib_options opt;
			m_mirror_calib_options_init(&opt);
			opt.mode = M_MIRROR_CALIB_MODE_PITCH;
			m_mirror_calib_result r;
			REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
			std::printf(
			    "[mirror_calib] mode (b) pitch %.1f deg %s: got %.3f deg, |dT| %.2f mm, rms %.2f mm, cond "
			    "%.3g\n",
			    pitch, n == &none ? "noise-free" : "noisy", r.pitch_deg, t_err_xy(r, p) * 1e3,
			    r.rms_m * 1e3, r.condition);
			if (n == &none) {
				CHECK(r.pitch_deg == Catch::Approx(pitch).margin(1e-6));
				CHECK(t_err_xy(r, p) < 1e-7);
			} else {
				CHECK(r.pitch_deg == Catch::Approx(pitch).margin(1.0));
				CHECK(t_err_xy(r, p) < 0.010);
			}
			CHECK(r.yaw_deg == Catch::Approx(0.0).margin(1e-9));
			CHECK(r.params == 3);
			CHECK(r.rank == 3);
		}
	}
}

TEST_CASE("mirror calib: mode (a) on a pitched camera does not pass silently", "[mirror_calib]")
{
	std::mt19937 rng(7);
	const Pose p = make_pose(0.0, 0.20, 0.0, 12.0);
	std::vector<m_mirror_calib_sample> s;
	for (double z : {0.45, 0.80}) {
		for (auto d : five_dots()) {
			s.push_back(make_sample(p, d.first, d.second, z, Noise{}, rng));
		}
	}
	m_mirror_calib_result ra, rb;
	m_mirror_calib_solve(s.data(), (int)s.size(), nullptr, &ra);
	INFO(ra.reason);
	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	opt.mode = M_MIRROR_CALIB_MODE_PITCH;
	REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &rb));
	// The wrong model never passes silently: a large residual, or a refusal (here: an
	// eye point the R = I fit can only place behind the camera).
	CHECK((ra.status != M_MIRROR_CALIB_OK || ra.rms_m > 0.005));
	CHECK(rb.rms_m < 1e-8);
}

TEST_CASE("mirror calib: mode (c) recovers a full rotation and the focal scale", "[mirror_calib]")
{
	std::mt19937 rng(8);
	const Pose p = make_pose(0.05, 0.19, 0.0, 10.0, -6.0, 3.0);
	const double k_true = 1.07;
	// 3 x 3 grid at 15 % inset, two distances = 18 samples.
	std::vector<m_mirror_calib_sample> s;
	for (double z : {0.45, 0.80}) {
		for (double gx : {-0.21, 0.0, 0.21}) {
			for (double gy : {-0.12, 0.0, 0.12}) {
				auto smp = make_sample(p, gx, gy, z, Noise{}, rng);
				// The provider computed its direction with focal f0 while the true focal is k * f0:
				// it reports normalize(u_x, u_y, u_z / k), which the solver undoes with k.
				double u[3] = {smp.dir[0], smp.dir[1], smp.dir[2] / k_true};
				const double l = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
				for (int i = 0; i < 3; i++) {
					smp.dir[i] = u[i] / l;
				}
				s.push_back(smp);
			}
		}
	}
	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	opt.mode = M_MIRROR_CALIB_MODE_FULL;
	opt.solve_focal_scale = true;
	m_mirror_calib_result r;
	REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
	std::printf(
	    "[mirror_calib] mode (c): pitch %.4f yaw %.4f roll %.4f deg, k %.5f, |dT| %.4f mm, rms %.2g mm, "
	    "cond %.3g, %d iterations\n",
	    r.pitch_deg, r.yaw_deg, r.roll_deg, r.focal_scale, t_err_xy(r, p) * 1e3, r.rms_m * 1e3, r.condition,
	    r.iterations);
	CHECK(r.pitch_deg == Catch::Approx(10.0).margin(1e-3));
	CHECK(r.yaw_deg == Catch::Approx(-6.0).margin(1e-3));
	CHECK(r.roll_deg == Catch::Approx(3.0).margin(1e-3));
	CHECK(r.focal_scale == Catch::Approx(k_true).margin(1e-4));
	CHECK(t_err_xy(r, p) < 1e-5);
	CHECK(r.params == 6);
	CHECK(r.rank == 6);
}

/*
 * The Dashboard's procedure (five dots at a 30 % inset, a near pass then a far pass) on a laptop
 * panel whose top-bezel camera is turned 4 deg toward +X: why it solves mode (c) and not (b), and
 * why it needs the two distances.
 */
TEST_CASE("mirror calib: the near/far procedure recovers a yawed laptop camera in mode (c)", "[mirror_calib]")
{
	const double w = 0.30, h = 0.19;
	const std::vector<std::pair<double, double>> dots = {
	    {0, 0}, {-0.2 * w, 0}, {0.2 * w, 0}, {0, 0.2 * h}, {0, -0.2 * h}};
	const Pose p = make_pose(0.0, 0.105, 0.0, 2.0, 4.0, 0.5);
	auto procedure = [&](std::vector<double> zs, const Noise &n, std::mt19937 &rng) {
		std::vector<m_mirror_calib_sample> s;
		for (double z : zs) {
			for (auto d : dots) {
				s.push_back(make_sample(p, d.first, d.second, z, n, rng));
			}
		}
		return s;
	};
	auto solve = [](const std::vector<m_mirror_calib_sample> &s, m_mirror_calib_mode mode,
	                m_mirror_calib_result &r) {
		m_mirror_calib_options opt;
		m_mirror_calib_options_init(&opt);
		opt.mode = mode;
		opt.reject_threshold_m = 0.005;
		return m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r);
	};

	SECTION("noise-free: (c) is exact with two distances, refused at one")
	{
		std::mt19937 rng(21);
		m_mirror_calib_result r;
		REQUIRE(solve(procedure({0.35, 0.65}, Noise{}, rng), M_MIRROR_CALIB_MODE_FULL, r));
		CHECK(r.yaw_deg == Catch::Approx(4.0).margin(1e-4));
		CHECK(r.pitch_deg == Catch::Approx(2.0).margin(1e-4));
		CHECK(t_err_xy(r, p) < 1e-6);
		CHECK_FALSE(r.ill_conditioned);

		// One distance: rotation and translation trade off exactly. Refused, so the Dashboard
		// falls back (and its distance check keeps this from being reached).
		m_mirror_calib_result r1;
		CHECK_FALSE(solve(procedure({0.50, 0.50}, Noise{}, rng), M_MIRROR_CALIB_MODE_FULL, r1));
		CHECK(r1.status == M_MIRROR_CALIB_ERROR_DEGENERATE);

		// (b) has no yaw: the yaw lands in T (tens of millimetres), with no warning.
		m_mirror_calib_result rb;
		REQUIRE(solve(procedure({0.35, 0.65}, Noise{}, rng), M_MIRROR_CALIB_MODE_PITCH, rb));
		CHECK(t_err_xy(rb, p) > 0.020);
	}

	SECTION("noisy: (c) lands within ~1 cm; the focal scale is not worth solving here")
	{
		const Noise n{0.2, 0.002, 0.07, 1.0};
		std::vector<double> err_c, err_ck, yaw_c;
		int ill_ck = 0;
		for (unsigned seed = 0; seed < 40; seed++) {
			std::mt19937 rng(1000 + seed);
			const auto s = procedure({0.35, 0.65}, n, rng);
			m_mirror_calib_result r, rk;
			REQUIRE(solve(s, M_MIRROR_CALIB_MODE_FULL, r));
			err_c.push_back(t_err_xy(r, p));
			yaw_c.push_back(fabs(r.yaw_deg - 4.0));
			m_mirror_calib_options opt;
			m_mirror_calib_options_init(&opt);
			opt.mode = M_MIRROR_CALIB_MODE_FULL;
			opt.solve_focal_scale = true;
			opt.reject_threshold_m = 0.005;
			if (m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &rk)) {
				err_ck.push_back(t_err_xy(rk, p));
				ill_ck += rk.ill_conditioned ? 1 : 0;
			}
		}
		auto med = [](std::vector<double> v) {
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};
		std::printf(
		    "[mirror_calib] near/far procedure, 40 seeds: (c) median |dT| %.1f mm, median |dyaw| %.2f deg; "
		    "(c)+focal median |dT| %.1f mm, %d ill-conditioned\n",
		    med(err_c) * 1e3, med(yaw_c), err_ck.empty() ? 0.0 : med(err_ck) * 1e3, ill_ck);
		CHECK(med(err_c) < 0.010);
		CHECK(med(yaw_c) < 0.5);
		// With five dots the focal scale trades off against the rotation: many fits flag it.
		CHECK(ill_ck > 0);
	}
}

TEST_CASE("mirror calib: iris scale and outlier rejection", "[mirror_calib]")
{
	std::mt19937 rng(9);
	const Pose p = make_pose(-0.02, 0.18, 0.0, 0.0);
	Noise n{};
	n.iris_scale = 1.12; // the user's iris is smaller than the landmarker's model: it under-reads distance
	std::vector<m_mirror_calib_sample> s;
	int i = 0;
	for (auto d : five_dots()) {
		s.push_back(make_sample(p, d.first, d.second, 0.5 + 0.05 * i++, n, rng));
	}
	for (auto d : five_dots()) {
		s.push_back(make_sample(p, d.first, d.second, 0.7, n, rng));
	}
	// One sample captured before the head settled: 30 mm off its dot.
	{
		Noise bad = n;
		std::mt19937 r2(10);
		auto smp = make_sample(p, 0.03 + 0.0, 0.0, 0.6, bad, r2);
		smp.dot_m[0] = 0.0; // recorded as the centre dot
		s.push_back(smp);
	}
	m_mirror_calib_options opt;
	m_mirror_calib_options_init(&opt);
	m_mirror_calib_result r0;
	REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r0));
	CHECK(r0.max_m > 0.005);
	opt.reject_threshold_m = 0.005;
	m_mirror_calib_result r;
	REQUIRE(m_mirror_calib_solve(s.data(), (int)s.size(), &opt, &r));
	CHECK(r.rejected[s.size() - 1]);
	CHECK(r.used_count == (int)s.size() - 1);
	CHECK(r.rms_m < 1e-9);
	CHECK(t_err_xy(r, p) < 1e-9);
	CHECK(r.iris_scale == Catch::Approx(1.12).epsilon(1e-9));
}

TEST_CASE("mirror calib: the aligned eye point from a per-eye camera block", "[mirror_calib]")
{
	const float l[3] = {-0.1f, 0.0f, 1.0f}, r[3] = {0.1f, 0.0f, 1.0f};
	double d[3], dist;
	REQUIRE(m_mirror_calib_eye_point(M_MIRROR_CALIB_EYE_MID, l, 0.6f, r, 0.6f, d, &dist));
	CHECK(d[0] == Catch::Approx(0.0).margin(1e-7));
	CHECK(d[2] == Catch::Approx(1.0));
	CHECK(dist == Catch::Approx(0.6 * 1.0 / sqrt(1.01)).epsilon(1e-6));
	REQUIRE(m_mirror_calib_eye_point(M_MIRROR_CALIB_EYE_RIGHT, l, 0.6f, r, 0.5f, d, &dist));
	CHECK(d[0] == Catch::Approx(0.1 / sqrt(1.01)).epsilon(1e-6));
	CHECK(dist == Catch::Approx(0.5));
	enum m_mirror_calib_eye e;
	CHECK(m_mirror_calib_parse_eye("Left", &e));
	CHECK(e == M_MIRROR_CALIB_EYE_LEFT);
	enum m_mirror_calib_mode m;
	CHECK(m_mirror_calib_parse_mode("b", &m));
	CHECK(m == M_MIRROR_CALIB_MODE_PITCH);
	CHECK_FALSE(m_mirror_calib_parse_mode("d", &m));
}
