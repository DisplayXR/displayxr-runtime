// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The view rig's point mapping (XR_DXR_view_rig spec 4, #2189) is the
 *        shared core's eye mapping — one implementation in practice.
 *
 * Four groups:
 *  - EQUIVALENCE: across a sweep of canvases (full display, offset window,
 *    rotated canvas, portrait swap), rig poses and tunables, a point mapped by
 *    m_view_rig_map_canvas_point is BIT-IDENTICAL to the displayxr-common
 *    core's eye_world for an eye at that point (count 1, ipd = parallax = 1)
 *    for a display rig, and for a camera rig (spec 5) to the display core's
 *    eye_world under the core's own camera -> display conversion of that rig
 *    (the glass at the convergence plane, x V / H); and matches the core's
 *    two-eye output to float noise.
 *  - The canvas rebase is the step the view locate applies to eyes.
 *  - The issue's joint cases: an unscaled centred display rig is the identity;
 *    virtualDisplayHeight scale k scales positions and radii by k; a camera rig
 *    puts the glass at its convergence plane, scaled by its virtual screen
 *    height / canvas height (metersToVirtual about the nominal plane only at
 *    infinite convergence); no rig = unchanged;
 *    velocities and orientations follow the rig rotation.
 *  - RIG_LOCAL is canvas-relative (ADR-034 Amendment 7): a full-display canvas
 *    is unchanged bit for bit; a smaller canvas puts L at its centre + s L
 *    (s = canvas / display height) with radii and velocities times s, and the
 *    spec-4 joint is the canvas point s L through the rig.
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

const xrt_vec3 kZero = {0.0f, 0.0f, 0.0f};

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

TEST_CASE("view rig point: a camera-rig point is the core's glass, bit for bit", "[view_rig_point]")
{
	// Spec 5: the camera rig's glass is the shared core's camera -> display
	// conversion (plane + virtual height), mapped by the display core with
	// perspective 1. Convergence at infinity keeps the camera core's eye mapping.
	for (const auto &c : canvases()) {
		for (const auto &pose : poses()) {
			for (float m2v : {1.0f, 0.01f, 100.0f, 0.0f /* unset = 1 */}) {
				for (float invd : {0.0f, 2.0f, 0.5f}) {
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
						dxr_xrt_view v;
						if (invd > 0.0f) {
							dxr_camera_rig cr{};
							std::memcpy(&cr.pose, &pose, sizeof(cr.pose));
							cr.ipd_factor = 1.0f;
							cr.parallax_factor = 1.0f;
							cr.inv_convergence_distance = invd;
							cr.half_tan_vfov = 0.3249f;
							cr.m2v = m2v > 0.0f ? m2v : 1.0f;
							const dxr_rig_display_info info = {
							    c.height_m, c.width_m / c.height_m, c.nominal_z_m};
							dxr_display_rig dr{};
							dxr_view_rig_camera_to_display(&cr, &info, &dr);
							const dxr_display3d_tunables dt = {1.0f, 1.0f, 1.0f,
							                                   dr.virtual_display_height};
							xrt_pose glass;
							std::memcpy(&glass, &dr.pose, sizeof(glass));
							dxr_xrt_display3d_compute_views(&pc, 1, &nominal, &scr, &dt,
							                                &glass, &v);
							CHECK(bit_equal(mine, v.eye_world));

							// Closed form: pose * (s x, s y, s z - 1/invd), s = V / H,
							// V = 2 tan(vFOV/2) / invd.
							const float s = 2.0f * 0.3249f / invd / c.height_m;
							CHECK(m_view_rig_length_scale(&c, &rig) == Catch::Approx(s));
							xrt_vec3 local = {pc.x * s, pc.y * s, pc.z * s - 1.0f / invd},
							         expect;
							math_quat_rotate_vec3(&pose.orientation, &local, &expect);
							expect = {expect.x + pose.position.x,
							          expect.y + pose.position.y,
							          expect.z + pose.position.z};
							check_near(mine, expect, 1e-4f * (1.0f + s + 1.0f / invd));
						} else {
							const dxr_camera3d_tunables ct = {1.0f, 1.0f, invd, 0.3249f,
							                                  m2v};
							dxr_xrt_camera3d_compute_views(&pc, 1, &nominal, &scr, &ct,
							                               &pose, &v);
							CHECK(bit_equal(mine, v.eye_world));

							// Closed form: pose * (m2v * (x, y, z - nominal_z)).
							const float s = m2v > 0.0f ? m2v : 1.0f;
							CHECK(m_view_rig_length_scale(&c, &rig) == Catch::Approx(s));
							xrt_vec3 local = {pc.x * s, pc.y * s,
							                  (pc.z - c.nominal_z_m) * s},
							         expect;
							math_quat_rotate_vec3(&pose.orientation, &local, &expect);
							expect = {expect.x + pose.position.x,
							          expect.y + pose.position.y,
							          expect.z + pose.position.z};
							check_near(mine, expect, 1e-4f * (1.0f + s));
						}
					}
				}
			}
		}
	}
}

TEST_CASE("view rig point: a camera rig puts the glass at its convergence plane", "[view_rig_point]")
{
	// The camera-centric scene the fix was reported on: convergence 0.5 (2 dp),
	// a vertical FOV giving a ~0.58 m virtual screen, on a 15.6" panel whose
	// nominal viewer is 0.6 m away. Camera at the base origin, looking -Z.
	const m_view_rig_canvas c = canvas_full();
	const float conv = 0.5f;
	const float vfov = 1.05f; // ~60 deg
	const float htan = std::tan(vfov * 0.5f);
	const float V = 2.0f * htan * conv;
	const float s = V / c.height_m;
	m_view_rig rig{};
	rig.kind = M_VIEW_RIG_CAMERA;
	rig.pose = {{0, 0, 0, 1}, {0, 0, 0}};
	rig.m2v = 1.0f;
	rig.inv_convergence_distance = 1.0f / conv;
	rig.half_tan_vfov = htan;

	SECTION("the glass centre lands at z = -convergence, in front of the camera")
	{
		xrt_vec3 out;
		m_view_rig_map_canvas_point(&c, &rig, &kZero, &out);
		check_near(out, {0.0f, 0.0f, -conv}, 1e-5f);
	}
	SECTION("a glass point lands on the virtual screen, x V / H: the pixel it covers")
	{
		// The top-right corner of the panel is the top-right corner of the
		// frustum at the convergence plane.
		const xrt_vec3 corner = {c.width_m * 0.5f, c.height_m * 0.5f, 0.0f};
		xrt_vec3 out;
		m_view_rig_map_canvas_point(&c, &rig, &corner, &out);
		check_near(out, {corner.x * s, V * 0.5f, -conv}, 1e-5f);
		CHECK(out.y / -out.z == Catch::Approx(htan)); // on the frustum's top edge
	}
	SECTION("a hand 5 cm in front of the glass is s x 5 cm in front of the convergence plane")
	{
		const xrt_vec3 hand = {0.02f, -0.01f, 0.05f};
		xrt_vec3 out;
		m_view_rig_map_canvas_point(&c, &rig, &hand, &out);
		check_near(out, {0.02f * s, -0.01f * s, -conv + 0.05f * s}, 1e-5f);
		CHECK(m_view_rig_length_scale(&c, &rig) == Catch::Approx(s));
	}
	SECTION("m_view_rig_camera_glass reports the plane and the scale")
	{
		xrt_pose glass;
		float gs = 0.0f;
		REQUIRE(m_view_rig_camera_glass(&c, &rig, &glass, &gs));
		check_near(glass.position, {0.0f, 0.0f, -conv}, 1e-5f);
		CHECK(gs == Catch::Approx(s));
		m_view_rig at_inf = rig;
		at_inf.inv_convergence_distance = 0.0f;
		CHECK_FALSE(m_view_rig_camera_glass(&c, &at_inf, nullptr, nullptr));
		m_view_rig display = rig;
		display.kind = M_VIEW_RIG_DISPLAY;
		CHECK_FALSE(m_view_rig_camera_glass(&c, &display, nullptr, nullptr));
	}
	SECTION("metersToVirtual moves the eyes, not the glass")
	{
		m_view_rig scaled = rig;
		scaled.m2v = 25.0f;
		const xrt_vec3 hand = {0.03f, 0.02f, 0.04f};
		xrt_vec3 a, b;
		m_view_rig_map_canvas_point(&c, &rig, &hand, &a);
		m_view_rig_map_canvas_point(&c, &scaled, &hand, &b);
		check_near(a, b, 1e-5f);
	}
	SECTION("a posed camera: the glass is 1/convergenceDiopters along its forward axis")
	{
		m_view_rig posed = rig;
		posed.pose = {quat_axis_angle(0, 1, 0, 0.6f), {1.0f, 1.5f, 2.0f}};
		xrt_vec3 out;
		m_view_rig_map_canvas_point(&c, &posed, &kZero, &out);
		const xrt_vec3 fwd = {0.0f, 0.0f, -conv};
		xrt_vec3 want;
		math_quat_rotate_vec3(&posed.pose.orientation, &fwd, &want);
		check_near(out, {1.0f + want.x, 1.5f + want.y, 2.0f + want.z}, 1e-5f);
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
	SECTION("a camera rig at infinite convergence maps by metersToVirtual about the nominal viewer plane")
	{
		const float m = 20.0f;
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_CAMERA;
		rig.pose = {{0, 0, 0, 1}, {0.0f, 1.6f, 4.0f}};
		rig.m2v = m;
		rig.inv_convergence_distance = 0.0f;
		rig.half_tan_vfov = 0.3249f;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		check_near(out.pose.position,
		           {in.pose.position.x * m, 1.6f + in.pose.position.y * m,
		            4.0f + (in.pose.position.z - c.nominal_z_m) * m},
		           1e-4f);
		CHECK(radius == Catch::Approx(0.01f * m));
	}
	SECTION("a camera rig puts the glass at its convergence plane; radius and velocity x V / H")
	{
		const float conv = 0.5f, htan = 0.5774f; // 60 deg vFOV
		const float s = 2.0f * htan * conv / c.height_m;
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_CAMERA;
		rig.pose = {{0, 0, 0, 1}, {0.0f, 1.6f, 4.0f}};
		rig.m2v = 1.0f;
		rig.inv_convergence_distance = 1.0f / conv;
		rig.half_tan_vfov = htan;
		m_view_rig_map_relation(&c, &rig, &plane, &in, 0.01f, &out, &radius);
		check_near(
		    out.pose.position,
		    {in.pose.position.x * s, 1.6f + in.pose.position.y * s, 4.0f - conv + in.pose.position.z * s},
		    1e-4f);
		CHECK(radius == Catch::Approx(0.01f * s));
		check_near(out.linear_velocity,
		           {in.linear_velocity.x * s, in.linear_velocity.y * s, in.linear_velocity.z * s}, 1e-5f);
		check_near(out.angular_velocity, in.angular_velocity, 1e-6f);
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

namespace {

/*!
 * The oxr layer's RIG_LOCAL placement for a session that owns its canvas
 * (oxr_space.c rig_local_rebase + oxr_session.c hand joints): the rig-local
 * pose L placed RIGIDLY on the canvas plane P, then scaled by s about P.
 */
xrt_space_relation
place_rig_local(const m_view_rig_canvas &c,
                const m_view_rig *rig,
                const xrt_pose &phys_plane,
                const xrt_space_relation &L,
                float radius,
                float *out_radius,
                xrt_pose *out_plane = nullptr)
{
	xrt_pose P;
	m_view_rig_canvas_plane(&c, rig, &phys_plane, &P);
	if (out_plane != nullptr) {
		*out_plane = P;
	}
	xrt_space_relation rigid = L;
	math_pose_transform(&P, &L.pose, &rigid.pose);
	math_quat_rotate_vec3(&P.orientation, &L.linear_velocity, &rigid.linear_velocity);
	math_quat_rotate_vec3(&P.orientation, &L.angular_velocity, &rigid.angular_velocity);
	xrt_space_relation out;
	m_view_rig_scale_relation(&P.position, m_view_rig_canvas_scale(&c), &rigid, radius, &out, out_radius);
	return out;
}

} // namespace

TEST_CASE("RIG_LOCAL is canvas-relative: the canvas scale", "[view_rig_point][rig_local_canvas]")
{
	m_view_rig_canvas c = canvas_full();
	CHECK(m_view_rig_canvas_scale(&c) == 1.0f); // display height unknown
	c.display_height_m = kH;
	CHECK(m_view_rig_canvas_scale(&c) == 1.0f); // the full display
	c.display_height_m = kH * (1.0f + 5e-5f);
	CHECK(m_view_rig_canvas_scale(&c) == 1.0f); // float noise between two sources snaps to 1
	c.height_m = kH * 0.5f;
	c.display_height_m = kH;
	CHECK(m_view_rig_canvas_scale(&c) == Catch::Approx(0.5f));
	c.valid = false;
	CHECK(m_view_rig_canvas_scale(&c) == 1.0f);
}

TEST_CASE("RIG_LOCAL is canvas-relative: placement (ADR-034 Amendment 7)", "[view_rig_point][rig_local_canvas]")
{
	const xrt_pose phys = {{0, 0, 0, 1}, {0, 0, 0}}; // the physical plane at the head's tracking origin
	const xrt_space_relation L = joint({0.04f, -0.03f, 0.25f});
	const float r = 0.01f;

	SECTION("a full-display canvas is unchanged bit for bit")
	{
		m_view_rig_canvas c = canvas_full();
		c.display_height_m = kH;
		REQUIRE(m_view_rig_canvas_scale(&c) == 1.0f);

		// No rig: the plane is the physical plane, and nothing is scaled.
		xrt_pose P;
		m_view_rig_canvas_plane(&c, nullptr, &phys, &P);
		CHECK(std::memcmp(&P, &phys, sizeof(P)) == 0);
		xrt_space_relation out;
		float out_r = 0.0f;
		m_view_rig_scale_relation(&P.position, 1.0f, &L, r, &out, &out_r);
		CHECK(std::memcmp(&out, &L, sizeof(out)) == 0);
		CHECK(out_r == r);

		// A rig: the canvas plane is exactly the plane the pre-amendment
		// runtime used (display point 0 through the rig).
		m_view_rig rig{};
		rig.kind = M_VIEW_RIG_DISPLAY;
		rig.pose = {quat_axis_angle(0, 1, 0, 0.4f), {0.4f, 0.3f, -0.35f}};
		rig.virtual_display_height = c.height_m * 3.0f;
		rig.perspective_factor = 1.0f;
		const xrt_pose posed = {quat_axis_angle(1, 0, 0, 0.1f), {0.0f, -1.6f, 0.2f}};
		xrt_space_relation at_plane{};
		at_plane.pose = posed;
		xrt_space_relation old_plane;
		m_view_rig_map_relation(&c, &rig, &posed, &at_plane, 0.0f, &old_plane, nullptr);
		m_view_rig_canvas_plane(&c, &rig, &posed, &P);
		CHECK(std::memcmp(&P, &old_plane.pose, sizeof(P)) == 0);
	}

	SECTION("a half-height canvas in a corner: L lands at centre + s L, radius and velocity scaled")
	{
		m_view_rig_canvas c = canvas_full();
		c.width_m = kW * 0.5f;
		c.height_m = kH * 0.5f;
		c.center = {kW * 0.25f, kH * 0.25f, 0.0f}; // the top-right quarter of the panel
		c.display_height_m = kH;
		const float s = m_view_rig_canvas_scale(&c);
		REQUIRE(s == Catch::Approx(0.5f));

		float out_r = 0.0f;
		const xrt_space_relation out = place_rig_local(c, nullptr, phys, L, r, &out_r);
		check_near(out.pose.position,
		           {c.center.x + s * L.pose.position.x, c.center.y + s * L.pose.position.y,
		            c.center.z + s * L.pose.position.z},
		           1e-6f);
		CHECK(out_r == Catch::Approx(r * s));
		check_near(out.linear_velocity,
		           {L.linear_velocity.x * s, L.linear_velocity.y * s, L.linear_velocity.z * s}, 1e-6f);
		// Orientations are not scaled (nor rotated, on a flat canvas).
		CHECK(std::memcmp(&out.pose.orientation, &L.pose.orientation, sizeof(xrt_quat)) == 0);
		check_near(out.angular_velocity, L.angular_velocity, 1e-6f);

		// The point at the physical display centre (L = 0) lands at the canvas centre.
		const xrt_space_relation at0 = place_rig_local(c, nullptr, phys, joint({0, 0, 0}), r, &out_r);
		check_near(at0.pose.position, c.center, 1e-7f);
	}

	SECTION("a rotated canvas: L is read in the canvas axes")
	{
		m_view_rig_canvas c = canvas_full();
		c.height_m = kH * 0.5f;
		c.width_m = kW * 0.5f;
		c.center = {-0.05f, 0.02f, 0.01f};
		c.orientation = quat_axis_angle(0, 1, 0, 0.3f);
		c.has_orientation = true;
		c.display_height_m = kH;
		const float s = m_view_rig_canvas_scale(&c);
		float out_r = 0.0f;
		const xrt_space_relation out = place_rig_local(c, nullptr, phys, L, r, &out_r);
		// Back into the canvas frame: s L.
		xrt_vec3 back;
		m_view_rig_canvas_rebase(&c, &out.pose.position, &back);
		check_near(back, {s * L.pose.position.x, s * L.pose.position.y, s * L.pose.position.z}, 1e-6f);
	}

	SECTION("with a rig, the spec-4 joint is the canvas point s L through the rig")
	{
		m_view_rig_canvas c = canvas_full();
		c.width_m = kW * 0.5f;
		c.height_m = kH * 0.5f;
		c.center = {-kW * 0.25f, -kH * 0.25f, 0.0f};
		c.display_height_m = kH;
		const float s = m_view_rig_canvas_scale(&c);

		for (const float k : {1.0f, 4.0f}) {
			CAPTURE(k);
			m_view_rig rig{};
			rig.kind = M_VIEW_RIG_DISPLAY;
			rig.pose = {quat_axis_angle(0, 1, 0, 0.4f), {0.4f, 0.3f, -0.35f}};
			rig.virtual_display_height = c.height_m * k;
			rig.perspective_factor = 1.0f;

			// Base space (action spaces, joints without a descriptor).
			float base_r = 0.0f;
			xrt_pose P;
			const xrt_space_relation base = place_rig_local(c, &rig, phys, L, r, &base_r, &P);

			// Spec 4: undo P o inverse(canvas pose), then the ordinary mapping.
			xrt_pose cp, cp_inv, undo;
			m_view_rig_canvas_pose(&c, &cp);
			math_pose_invert(&cp, &cp_inv);
			math_pose_transform(&P, &cp_inv, &undo);
			xrt_space_relation scoped;
			float scoped_r = 0.0f;
			m_view_rig_map_relation(&c, &rig, &undo, &base, base_r, &scoped, &scoped_r);

			const xrt_vec3 sL = {s * L.pose.position.x, s * L.pose.position.y, s * L.pose.position.z};
			xrt_vec3 want;
			m_view_rig_map_canvas_point(&c, &rig, &sL, &want);
			check_near(scoped.pose.position, want, 1e-5f);
			CHECK(scoped_r == Catch::Approx(r * s * m_view_rig_length_scale(&c, &rig)));
			// Orientation: the rig's rotation of L's, nothing else.
			xrt_quat want_q;
			math_quat_rotate(&rig.pose.orientation, &L.pose.orientation, &want_q);
			CHECK(std::fabs(scoped.pose.orientation.x - want_q.x) < 1e-5f);
			CHECK(std::fabs(scoped.pose.orientation.y - want_q.y) < 1e-5f);
			CHECK(std::fabs(scoped.pose.orientation.z - want_q.z) < 1e-5f);
			CHECK(std::fabs(scoped.pose.orientation.w - want_q.w) < 1e-5f);

			if (k == 1.0f) {
				// An unscaled rig: base-space hands ARE the rig-frame hands.
				check_near(base.pose.position, scoped.pose.position, 1e-5f);
				CHECK(base_r == Catch::Approx(scoped_r));
			}
		}
	}
}
