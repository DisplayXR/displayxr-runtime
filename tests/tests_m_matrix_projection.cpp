// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for the two infinite-far / reversed-depth projection builders (#1580).
 * @ingroup tests
 *
 * `math_matrix_4x4_projection_vulkan_infinite_reverse()` hard-wires Vulkan's
 * Y-DOWN clip space (`vulkan_projection_space_y = true`), which negates row 1
 * of the matrix. The D3D11 quad / cylinder paths consumed it anyway, and
 * D3D11 NDC is Y-UP -- so every quad landed mirrored about the view's
 * horizontal centre line relative to the projection layer's identity blit.
 *
 * `math_matrix_4x4_projection_d3d_infinite_reverse()` is the Y-UP sibling.
 * The two share one body, and what this file pins is exactly the invariant
 * that makes sharing safe: they are EXACT Y-negations of each other, and
 * nothing else about them differs.
 *
 * Matrix layout reminder: xrt_matrix_4x4::v is column-major, so
 *   v[0]  = a11   v[5]  = a22   v[8]  = a31   v[9]  = a32
 *   v[10] = a33   v[11] = a34   v[14] = a43
 * and clip = (a11*x + a31*z, a22*y + a32*z, a33*z + a43*w, a34*z).
 */

#include "catch_amalgamated.hpp"

#include "math/m_api.h"
#include "xrt/xrt_defines.h"

#include <cmath>

using Catch::Approx;

namespace {

constexpr float kEps = 1e-5f;

constexpr float
deg(float d)
{
	return d * 3.14159265358979323846f / 180.0f;
}

xrt_fov
make_fov(float up_deg, float down_deg, float left_deg, float right_deg)
{
	struct xrt_fov fov = {};
	fov.angle_left = deg(left_deg);
	fov.angle_right = deg(right_deg);
	fov.angle_up = deg(up_deg);
	fov.angle_down = deg(down_deg);
	return fov;
}

//! clip = M * (x, y, z, 1), with v column-major.
void
apply(const struct xrt_matrix_4x4 &m, float x, float y, float z, float out[4])
{
	for (int row = 0; row < 4; row++) {
		out[row] = m.v[0 + row] * x + //
		           m.v[4 + row] * y + //
		           m.v[8 + row] * z + //
		           m.v[12 + row] * 1.0f;
	}
}

float
ndc_y(const struct xrt_matrix_4x4 &m, float x, float y, float z)
{
	float clip[4];
	apply(m, x, y, z, clip);
	REQUIRE(std::fabs(clip[3]) > 1e-6f);
	return clip[1] / clip[3];
}

//! The asymmetric, entirely-below-axis frustum a 3D display actually hands us.
xrt_fov
asymmetric_fov()
{
	return make_fov(-0.3f, -18.2f, -13.3f, 18.6f);
}

} // namespace


TEST_CASE("m_matrix_projection: symmetric fov a22 sign and magnitude")
{
	struct xrt_fov fov = make_fov(45.0f, -45.0f, -45.0f, 45.0f);

	struct xrt_matrix_4x4 vk = {}, d3d = {};
	math_matrix_4x4_projection_vulkan_infinite_reverse(&fov, 0.1f, &vk);
	math_matrix_4x4_projection_d3d_infinite_reverse(&fov, 0.1f, &d3d);

	// Vulkan clip space is Y-down: row 1 is negated, so a22 < 0.
	CHECK(vk.v[5] < 0.0f);
	// D3D11 / D3D12 / Metal / GL clip space is Y-up: a22 > 0.
	CHECK(d3d.v[5] > 0.0f);
	// Same frustum, same magnitude.
	CHECK(std::fabs(vk.v[5]) == Approx(std::fabs(d3d.v[5])).margin(kEps));
	CHECK(d3d.v[5] == Approx(-vk.v[5]).margin(kEps));
}

TEST_CASE("m_matrix_projection: the X and Z rows are identical between the two")
{
	struct xrt_fov fov = asymmetric_fov();

	struct xrt_matrix_4x4 vk = {}, d3d = {};
	math_matrix_4x4_projection_vulkan_infinite_reverse(&fov, 0.1f, &vk);
	math_matrix_4x4_projection_d3d_infinite_reverse(&fov, 0.1f, &d3d);

	// Row 0 (X) -- a11 and a31.
	CHECK(d3d.v[0] == Approx(vk.v[0]).margin(kEps));
	CHECK(d3d.v[8] == Approx(vk.v[8]).margin(kEps));
	// Row 2 (Z) -- a33 and a43, and row 3 (W) -- a34.
	CHECK(d3d.v[10] == Approx(vk.v[10]).margin(kEps));
	CHECK(d3d.v[14] == Approx(vk.v[14]).margin(kEps));
	CHECK(d3d.v[11] == Approx(vk.v[11]).margin(kEps));

	// Every other entry matches too; only row 1 (a22 at v[5], a32 at v[9])
	// is allowed to differ.
	for (int i = 0; i < 16; i++) {
		if (i == 5 || i == 9) {
			continue;
		}
		INFO("element " << i);
		CHECK(d3d.v[i] == Approx(vk.v[i]).margin(kEps));
	}
	CHECK(d3d.v[9] == Approx(-vk.v[9]).margin(kEps));
}

TEST_CASE("m_matrix_projection: asymmetric fov, up is up under d3d and down under vulkan")
{
	struct xrt_fov fov = asymmetric_fov();

	struct xrt_matrix_4x4 vk = {}, d3d = {};
	math_matrix_4x4_projection_vulkan_infinite_reverse(&fov, 0.1f, &vk);
	math_matrix_4x4_projection_d3d_infinite_reverse(&fov, 0.1f, &d3d);

	// Two view-space points, 3 m out, symmetric about the view axis.
	const float kZ = -3.0f;
	const float kY = 0.3875f;

	const float d3d_hi = ndc_y(d3d, 0.0f, +kY, kZ);
	const float d3d_lo = ndc_y(d3d, 0.0f, -kY, kZ);
	const float vk_hi = ndc_y(vk, 0.0f, +kY, kZ);
	const float vk_lo = ndc_y(vk, 0.0f, -kY, kZ);

	// Y-up clip space: the higher world point lands higher in NDC.
	CHECK(d3d_hi > d3d_lo);
	// Y-down clip space: the ordering inverts. This is the mirror the
	// D3D11 quad path was drawing (#1580).
	CHECK(vk_hi < vk_lo);
}

TEST_CASE("m_matrix_projection: the two conventions are exact Y-negations")
{
	struct xrt_fov fov = asymmetric_fov();

	struct xrt_matrix_4x4 vk = {}, d3d = {};
	math_matrix_4x4_projection_vulkan_infinite_reverse(&fov, 0.1f, &vk);
	math_matrix_4x4_projection_d3d_infinite_reverse(&fov, 0.1f, &d3d);

	// A spread of points, including off-axis X and several depths.
	const float points[][3] = {
	    {0.0f, +0.3875f, -3.0f}, //
	    {0.0f, -0.3875f, -3.0f}, //
	    {0.0f, 0.0f, -3.0f},     //
	    {+0.5f, +0.25f, -1.0f},  //
	    {-0.5f, -0.25f, -1.0f},  //
	    {+0.2f, +1.5f, -10.0f},  //
	    {-1.2f, -0.7f, -0.5f},   //
	};

	for (const auto &p : points) {
		INFO("point (" << p[0] << ", " << p[1] << ", " << p[2] << ")");

		float clip_vk[4], clip_d3d[4];
		apply(vk, p[0], p[1], p[2], clip_vk);
		apply(d3d, p[0], p[1], p[2], clip_d3d);

		// X, Z and W are untouched by the convention.
		CHECK(clip_d3d[0] == Approx(clip_vk[0]).margin(kEps));
		CHECK(clip_d3d[2] == Approx(clip_vk[2]).margin(kEps));
		CHECK(clip_d3d[3] == Approx(clip_vk[3]).margin(kEps));

		// Y is exactly negated -- in clip space and therefore in NDC.
		CHECK(clip_d3d[1] == Approx(-clip_vk[1]).margin(kEps));
		CHECK(ndc_y(d3d, p[0], p[1], p[2]) == Approx(-ndc_y(vk, p[0], p[1], p[2])).margin(kEps));
	}
}
