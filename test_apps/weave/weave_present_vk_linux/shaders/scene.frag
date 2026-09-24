// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// weave_present_vk_linux stereo test scene, as ONE squeezed side-by-side pair
// in a window-sized target: the left half is the left eye's view of the whole
// window, the right half the right eye's.
//
// Every pixel is ray-cast from its eye through the point it represents on the
// window's canvas (a rectangle ON the display plane, in display-space metres).
// That is the off-axis (Kooima) projection by construction, so the stereo is
// geometrically right for a window anywhere on the panel with no matrix maths.
//
//  - a rotating cube centred on the canvas, straddling the display plane;
//  - a checkered back wall behind the display plane (depth cue);
//  - a colour-bar strip along the bottom, AT the display plane (zero
//    disparity: identical in both halves);
//  - a 1-px white border round each half, so a squeeze / crop / offset error
//    in the pipeline shows as a broken or doubled edge.
//
// Regenerate the embedded SPIR-V with ./compile.sh after editing.
#version 450

layout(location = 0) out vec4 o_color;

layout(push_constant) uniform Scene
{
	vec4 eye_l;  // xyz: left eye, display space (m, y up, +z towards the viewer)
	vec4 eye_r;  // xyz: right eye
	vec4 canvas; // x, y: canvas TOP-LEFT on the display plane (m); z, w: width, height (m)
	vec4 cube;   // xyz: cube centre (m); w: half edge (m)
	vec4 misc;   // x: angle (rad); y, z: target width, height (px); w: colour-bar strip fraction
}
pc;

const vec3 kBars[8] = vec3[8](vec3(1.0, 1.0, 1.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0),
                              vec3(1.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 0.0));

const vec3 kFaces[6] = vec3[6](vec3(0.90, 0.25, 0.20), vec3(0.20, 0.75, 0.30), vec3(0.25, 0.45, 0.95),
                               vec3(0.95, 0.80, 0.20), vec3(0.80, 0.30, 0.85), vec3(0.20, 0.80, 0.85));

mat3
rot(float a)
{
	float c = cos(a), s = sin(a);
	float c2 = cos(a * 0.61), s2 = sin(a * 0.61);
	mat3 ry = mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
	mat3 rx = mat3(1.0, 0.0, 0.0, 0.0, c2, s2, 0.0, -s2, c2);
	return ry * rx;
}

void
main()
{
	const int w = int(pc.misc.y);
	const int h = int(pc.misc.z);
	const int ew = w / 2;
	const ivec2 p = ivec2(gl_FragCoord.xy);
	const int eye = p.x < ew ? 0 : 1;
	const int lx = p.x - eye * ew;

	// 1-px border round each eye's half.
	if (lx == 0 || lx == ew - 1 || p.y == 0 || p.y == h - 1) {
		o_color = vec4(1.0);
		return;
	}

	const float u = (float(lx) + 0.5) / float(ew);
	const float v = (float(p.y) + 0.5) / float(h);

	// Colour bars at the display plane: same pixels in both eyes.
	if (v > 1.0 - pc.misc.w) {
		int bar = clamp(int(u * 8.0), 0, 7);
		o_color = vec4(kBars[bar], 1.0);
		return;
	}

	const vec3 e = eye == 0 ? pc.eye_l.xyz : pc.eye_r.xyz;
	const vec3 target = vec3(pc.canvas.x + u * pc.canvas.z, pc.canvas.y - v * pc.canvas.w, 0.0);
	const vec3 d = normalize(target - e);

	// Background: a checkered wall behind the display plane.
	vec3 col = vec3(0.10, 0.11, 0.14);
	const float wall_z = -pc.canvas.w * 0.6;
	if (d.z < 0.0) {
		float t = (wall_z - e.z) / d.z;
		vec3 q = e + t * d;
		float cell = pc.canvas.w / 6.0;
		ivec2 c = ivec2(floor((q.xy - vec2(pc.canvas.x, pc.canvas.y)) / cell));
		col = ((c.x + c.y) & 1) == 0 ? vec3(0.16, 0.17, 0.21) : vec3(0.08, 0.09, 0.11);
	}

	// Rotating cube (slab test in the cube's frame).
	const mat3 r = rot(pc.misc.x);
	const vec3 ro = transpose(r) * (e - pc.cube.xyz);
	const vec3 rd = transpose(r) * d;
	const vec3 inv = 1.0 / rd;
	const vec3 t0 = (-vec3(pc.cube.w) - ro) * inv;
	const vec3 t1 = (vec3(pc.cube.w) - ro) * inv;
	const vec3 tmin = min(t0, t1);
	const vec3 tmax = max(t0, t1);
	const float tn = max(max(tmin.x, tmin.y), tmin.z);
	const float tf = min(min(tmax.x, tmax.y), tmax.z);
	if (tn > 0.0 && tn < tf) {
		vec3 n;
		int face;
		if (tn == tmin.x) {
			n = vec3(-sign(rd.x), 0.0, 0.0);
			face = rd.x > 0.0 ? 0 : 1;
		} else if (tn == tmin.y) {
			n = vec3(0.0, -sign(rd.y), 0.0);
			face = rd.y > 0.0 ? 2 : 3;
		} else {
			n = vec3(0.0, 0.0, -sign(rd.z));
			face = rd.z > 0.0 ? 4 : 5;
		}
		const vec3 hit = ro + tn * rd;
		// Dark edges make the silhouette and the disparity easy to read.
		const vec3 a = abs(hit) / pc.cube.w;
		const float edge = step(0.92, a.x) * step(0.92, a.y) + step(0.92, a.y) * step(0.92, a.z) +
		                   step(0.92, a.x) * step(0.92, a.z);
		const vec3 nw = r * n;
		const float lambert = 0.35 + 0.65 * max(dot(nw, normalize(vec3(0.4, 0.7, 0.6))), 0.0);
		col = kFaces[face] * lambert;
		if (edge > 0.0) {
			col *= 0.25;
		}
	}
	o_color = vec4(col, 1.0);
}
