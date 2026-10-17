// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The view rig's point mapping (XR_DXR_view_rig spec 4, #2189) is the
 *        shared core's eye mapping — one implementation in practice.
 *
 * Three groups:
 *  - EQUIVALENCE: across a sweep of canvases (full display, offset window,
 *    rotated canvas, portrait swap), rig poses and tunables, a point mapped by
 *    m_view_rig_map_canvas_point is BIT-IDENTICAL to the displayxr-common
 *    core's eye_world for an eye at that point (count 1, ipd = parallax = 1),
 *    for both rig kinds; and matches the core's two-eye output to float noise.
 *  - The canvas rebase is the step the view locate applies to eyes.
 *  - The issue's joint cases: an unscaled centred display rig is the identity;
 *    virtualDisplayHeight scale k scales positions and radii by k; a camera rig
 *    maps by metersToVirtual about the nominal plane; no rig = unchanged;
 *    velocities and orientations follow the rig rotation.
 */

#include "math/m_api.h"
#include "math/m_view_rig_point.h"

#include "displayxr_math_xrt.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr float kW = 0.3442f; // a 15.6" panel
constexpr float kH = 0.1936f;

struct xrt_quat
quat_axis_angle(float x, float y, float z, float angle)
{
	const float s = std::sin(angle * 0.5f);
	return {x * s, y * s, z * s, std::cos(angle * 0.5f)};
}

m_view_rig_canvas
canvas_full()
{
	m_view_rig_canvas c{};
	c.valid = true;
	c.width_m = kW;
	c.height_m = kH;
	c.orientation = {0, 0, 0, 1};
	c.nominal_y_m = 0.0f;
	c.nominal_z_m = 0.6f;
	return c;
}

std::vector<m_view_rig_canvas>
canvases()
{
	std::vector<m_view_rig_canvas> v;
	v.push_back(canvas_full());
	m_view_rig_canvas win = canvas_full(); // an offset window
	win.width_m = 0.18f;
	win.height_m = 0.11f;
	win.center = {0.05f, -0.02f, 0.0f};
	v.push_back(win);
	m_view_rig_canvas rot = win; // a canvas rotated on the panel (workspace tile)
	rot.orientation = quat_axis_angle(0, 1, 0, 0.2f);
	rot.has_orientation = true;
	v.push_back(rot);
	m_view_rig_canvas portrait = canvas_full(); // the #499 portrait swap
	portrait.width_m = kH;
	portrait.height_m = kW;
	v.push_back(portrait);
	return v;
}

std::vector<xrt_pose>
poses()
{
	return {
	    {{0, 0, 0, 1}, {0, 0, 0}},
	    {quat_axis_angle(0, 1, 0, 0.7f), {1.5f, -0.25f, -3.0f}},
	    {quat_axis_angle(1, 0, 0, -0.3f), {0.0f, 2.0f, 0.5f}},
	};
}

std::vector<xrt_vec3>
points()
{
	return {{0, 0, 0}, {0.032f, 0.01f, 0.6f}, {-0.1f, 0.05f, 0.25f}, {0.12f, -0.08f, -0.04f}, {0.0f, 0.0f, 1.2f}};
}

bool
bit_equal(const xrt_vec3 &a, const xrt_vec3 &b)
{
	return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void
check_near(const xrt_vec3 &a, const xrt_vec3 &b, float tol)
{
	CHECK(std::fabs(a.x - b.x) <= tol);
	CHECK(std::fabs(a.y - b.y) <= tol);
	CHECK(std::fabs(a.z - b.z) <= tol);
}

} // namespace


TEST_CASE("view rig point: a display-rig point is the core's eye, bit for bit", "[view_rig_point]")
{
	for (const auto &c : canvases()) {
		for (const auto &pose : poses()) {
			for (float vdh : {c.height_m, 1.0f, 0.5f, 7.0f}) {
				for (float persp : {1.0f, 0.5f, 2.0f}) {
					m_view_rig rig{};
					rig.kind = M_VIEW_RIG_DISPLAY;
					rig.pose = pose;
					rig.virtual_display_height = vdh;
					rig.perspective_factor = persp;
					for (const auto &p : points()) {
						xrt_vec3 pc;
						m_view_rig_canvas_rebase(&c, &p, &pc);
						xrt_vec3 mine;
						m_view_rig_map_canvas_point(&c, &rig, &pc, &mine);

						const xrt_vec3 nominal = {0, c.nominal_y_m, c.nominal_z_m};
						const dxr_screen scr = {c.width_m, c.height_m};
						const dxr_display3d_tunables dt = {1.0f, 1.0f, persp, vdh};
						dxr_xrt_view v;
						dxr_xrt_display3d_compute_views(&pc, 1, &nominal, &scr, &dt, &pose, &v);
						CHECK(bit_equal(mine, v.eye_world));

						// Closed form: pose * (persp * vdh / h * p_canvas), to float noise.
						const float s = persp * vdh / c.height_m;
						CHECK(m_view_rig_length_scale(&c, &rig) == Catch::Approx(s));
						xrt_vec3 scaled = {pc.x * s, pc.y * s, pc.z * s}, expect;
						math_quat_rotate_vec3(&pose.orientation, &scaled, &expect);
						expect = {expect.x + pose.position.x, expect.y + pose.position.y,
						          expect.z + pose.position.z};
						check_near(mine, expect, 1e-4f * (1.0f + s));
					}
				}
			}
		}
	}
}

TEST_CASE("view rig point: a camera-rig point is the core's eye, bit for bit", "[view_rig_point]")
{
	for (const auto &c : canvases()) {
		for (const auto &pose : poses()) {
			for (float m2v : {1.0f, 0.01f, 100.0f, 0.0f /* unset = 1 */}) {
				for (float invd : {0.0f, 2.0f}) {
					m_view_rig rig{};
					rig.kind = M_VIEW_RIG_CAMERA;
					rig.pose = pose;
					rig.m2v = m2v;
					rig.inv_convergence_distance = invd;
					rig.half_tan_vfov = 0.3249f;
					for (const auto &p : points()) {
						xrt_vec3 pc;
						m_view_rig_canvas_rebase(&c, &p, &pc);
						xrt_vec3 mine;
						m_view_rig_map_canvas_point(&c, &rig, &pc, &mine);

						const xrt_vec3 nominal = {0, c.nominal_y_m, c.nominal_z_m};
						const dxr_screen scr = {c.width_m, c.height_m};
						const dxr_camera3d_tunables ct = {1.0f, 1.0f, invd, 0.3249f, m2v};
						dxr_xrt_view v;
						dxr_xrt_camera3d_compute_views(&pc, 1, &nominal, &scr, &ct, &pose, &v);
						CHECK(bit_equal(mine, v.eye_world));

						// Closed form: pose * (m2v * (x, y, z - nominal_z)); convergence never
						// moves a point.
						const float s = m2v > 0.0f ? m2v : 1.0f;
						CHECK(m_view_rig_length_scale(&c, &rig) == Catch::Approx(s));
						xrt_vec3 local = {pc.x * s, pc.y * s, (pc.z - c.nominal_z_m) * s},
						         expect;
						math_quat_rotate_vec3(&pose.orientation, &local, &expect);
						expect = {expect.x + pose.position.x, expect.y + pose.position.y,
						          expect.z + pose.position.z};
						check_near(mine, expect, 1e-4f * (1.0f + s));
					}
				}
			}
		}
	}
}

TEST_CASE("view rig point: a point matches the core's EYE PAIR at the same place", "[view_rig_point]")
{
	// The views come from a two-eye call; a joint at an eye's position must land
	// on that eye (float noise only — the pair's centroid arithmetic rounds).
	for (const auto &c : canvases()) {
		for (const auto &pose : poses()) {
			const xrt_vec3 eyes_display[2] = {{-0.032f, 0.01f, 0.6f}, {0.032f, 0.01f, 0.6f}};
			xrt_vec3 eyes[2];
			for (int i = 0; i < 2; i++) {
				m_view_rig_canvas_rebase(&c, &eyes_display[i], &eyes[i]);
			}
			const xrt_vec3 nominal = {0, c.nominal_y_m, c.nominal_z_m};
			const dxr_screen scr = {c.width_m, c.height_m};
			m_view_rig rig{};
			rig.kind = M_VIEW_RIG_DISPLAY;
			rig.pose = pose;
			rig.virtual_display_height = 1.0f;
			rig.perspective_factor = 1.0f;
			const dxr_display3d_tunables dt = {1.0f, 1.0f, 1.0f, 1.0f};
			dxr_xrt_view v[2];
			dxr_xrt_display3d_compute_views(eyes, 2, &nominal, &scr, &dt, &pose, v);
			for (int i = 0; i < 2; i++) {
				xrt_vec3 mine;
				m_view_rig_map_canvas_point(&c, &rig, &eyes[i], &mine);
				check_near(mine, v[i].eye_world, 1e-5f * (1.0f / c.height_m));
			}
		}
	}
}

TEST_CASE("view rig point: the canvas rebase", "[view_rig_point]")
{
	auto c = canvases()[2]; // offset + rotated
	const xrt_vec3 p = {0.1f, 0.0f, 0.3f};
	xrt_vec3 out;
	m_view_rig_canvas_rebase(&c, &p, &out);
	// Undo it: rotate back, add the centre.
	xrt_vec3 back;
	math_quat_rotate_vec3(&c.orientation, &out, &back);
	check_near({back.x + c.center.x, back.y + c.center.y, back.z + c.center.z}, p, 1e-6f);
	c.has_orientation = false;
	m_view_rig_canvas_rebase(&c, &p, &out);
	CHECK(bit_equal(out, xrt_vec3{p.x - c.center.x, p.y - c.center.y, p.z - c.center.z}));
}

namespace {

xrt_space_relation
joint(const xrt_vec3 &pos)
{
	xrt_space_relation r{};
	r.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	r.pose = {quat_axis_angle(0, 0, 1, 0.4f), pos};
	r.linear_velocity = {0.1f, -0.2f, 0.05f};
	r.angular_velocity = {0.0f, 1.0f, 0.0f};
	return r;
}

} // namespace

TEST_CASE("view rig joints: the issue's cases", "[view_rig_point]")
{
	const m_view_rig_canvas c = canvas_full();
	const xrt_pose plane = {{0, 0, 0, 1}, {0, 0, 0}}; // LOCAL: panel centre
	const xrt_space_relation in = joint({0.04f, -0.03f, 0.25f});
	xrt_space_relation out;
	float radius = 0.0f;

	SECTION("no rig: unchanged")
	{
		m_view_rig none{};
		m_view_rig_map_relation(&c, &none, &plane, &in, 0.01f, &out, &radius);
		CHECK(std::memcmp(&out, &in, sizeof(out)) == 0);
		CHECK(radius == 0.01f);
	}
	SECTION("an unscaled display rig centred on the panel is the identity")
	{
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_DISPLAY;
		rig.pose = plane;
		rig.virtual_display_height = c.height_m; // m2v = 1
		rig.perspective_factor = 1.0f;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		check_near(out.pose.position, in.pose.position, 1e-6f);
		CHECK(out.pose.orientation.z == Catch::Approx(in.pose.orientation.z));
		CHECK(radius == Catch::Approx(0.01f));
		check_near(out.linear_velocity, in.linear_velocity, 1e-6f);
		CHECK(out.relation_flags == in.relation_flags);
	}
	SECTION("virtualDisplayHeight scale k: positions, radii and linear velocity scale by k")
	{
		const float k = 5.0f;
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_DISPLAY;
		rig.pose = plane;
		rig.virtual_display_height = c.height_m * k;
		rig.perspective_factor = 1.0f;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		check_near(out.pose.position, {in.pose.position.x * k, in.pose.position.y * k, in.pose.position.z * k},
		           1e-5f);
		CHECK(radius == Catch::Approx(0.01f * k));
		check_near(out.linear_velocity,
		           {in.linear_velocity.x * k, in.linear_velocity.y * k, in.linear_velocity.z * k}, 1e-5f);
		check_near(out.angular_velocity, in.angular_velocity, 1e-6f); // rotation-only: unscaled
	}
	SECTION("a camera rig maps by metersToVirtual about the nominal viewer plane")
	{
		const float m = 20.0f;
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_CAMERA;
		rig.pose = {{0, 0, 0, 1}, {0.0f, 1.6f, 4.0f}};
		rig.m2v = m;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		check_near(out.pose.position,
		           {in.pose.position.x * m, 1.6f + in.pose.position.y * m,
		            4.0f + (in.pose.position.z - c.nominal_z_m) * m},
		           1e-4f);
		CHECK(radius == Catch::Approx(0.01f * m));
	}
	SECTION("a rotated rig rotates orientation and both velocities")
	{
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_DISPLAY;
		rig.pose = {quat_axis_angle(0, 1, 0, 1.5707963f), {0, 0, 0}};
		rig.virtual_display_height = c.height_m;
		rig.perspective_factor = 1.0f;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		// +90 deg about Y: x -> -z, z -> x.
		check_near(out.pose.position, {in.pose.position.z, in.pose.position.y, -in.pose.position.x}, 1e-5f);
		check_near(out.linear_velocity, {in.linear_velocity.z, in.linear_velocity.y, -in.linear_velocity.x},
		           1e-5f);
		check_near(out.angular_velocity, in.angular_velocity, 1e-5f); // about Y: unchanged by a Y rotation
	}
	SECTION("a display plane that is not at the base origin is undone first")
	{
		const xrt_pose moved = {{0, 0, 0, 1}, {0.0f, 0.0f, -2.0f}}; // e.g. a STAGE base space
		xrt_space_relation in_moved = in;
		in_moved.pose.position.z += -2.0f; // the same physical joint, expressed in that base
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_DISPLAY;
		rig.pose = plane;
		rig.virtual_display_height = c.height_m;
		rig.perspective_factor = 1.0f;
		m_view_rig_map_relation(&c, &rig, &moved, &in_moved, 0.01f, &out, &radius);
		check_near(out.pose.position, in.pose.position, 1e-5f);
	}
}
