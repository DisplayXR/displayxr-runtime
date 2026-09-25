// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
//
// XR_KHR_composition_layer_equirect2 fragment shader (#1602), sampler2DArray (layered, #1601) source.
//
// The ray/sphere march of d3d_shared/comp_equirect2_shaders.h's PSMain and
// GL's FS_EQUIRECT2_BODY, line for line: GLSL, so a twin of GL's rather than a
// compile of the HLSL. Differences, all of FORM: the sphere scalars arrive in
// the packed `eq` matrix's w row (see equirect2.vert), and — unlike GL — there
// is no sample-Y flip, because a Vulkan image's v == 0 is its top row exactly
// as a D3D texture's is.
//
// Ends in `color * color_scale + color_bias`, like every compose shader, so
// OPAQUE_COVER's alpha-of-one is comp_layer_blend_fold_opaque_cover()'s job on
// the CPU and fragments outside the section, which discard, never reach it.
//
// No transfer function here: the pass's _SRGB attachment encodes on write
// (#1610), and the source view decodes on sample when the app asked for
// _SRGB (#1589).
//
// Kept in step with its equirect2.frag twin by hand; they differ only in the sampler
// declaration and DXR_EQ2_SAMPLE.

#version 450

layout(binding = 0) uniform sampler2DArray src_tex;
#define DXR_EQ2_SAMPLE(uv) texture(src_tex, vec3(uv, pc.params.x))

layout(push_constant) uniform ComposeParams {
	mat4 eq;          // packed ray basis + camera + sphere scalars (equirect2.vert)
	vec4 src_rect;    // post_transform: xy = offset, zw = scale (UV)
	vec4 params;      // x = array slice
	vec4 color_scale; // XR_KHR_composition_layer_color_scale_bias
	vec4 color_bias;  // ditto
} pc;

layout(location = 0) in vec3 in_camera_position;
layout(location = 1) in vec3 in_camera_ray;
layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

vec2 sphere_intersect(vec3 ray_origin, vec3 ray_direction, vec3 sphere_center, float r)
{
	vec3 ray_sphere_diff = ray_origin - sphere_center;
	float B = dot(ray_sphere_diff, ray_direction);
	vec3 QC = ray_sphere_diff - B * ray_direction;
	float H = r * r - dot(QC, QC);
	if (H < 0.0) {
		return vec2(-1.0, -1.0); // No intersection
	}
	H = sqrt(H);
	return vec2(-B - H, -B + H);
}

void main()
{
	const float radius = pc.eq[0].w;
	const float central_horizontal_angle = pc.eq[1].w;
	const float upper_vertical_angle = pc.eq[2].w;
	const float lower_vertical_angle = pc.eq[3].w;

	vec3 ray_origin = in_camera_position;
	vec3 ray_dir = normalize(in_camera_ray);
	vec3 dir_from_sph;

	// The CPU spells +INFINITY as a zero radius.
	if (radius == 0.0) {
		dir_from_sph = ray_dir;
	} else {
		vec2 distances = sphere_intersect(ray_origin, ray_dir, vec3(0.0), radius);
		// The ray misses the sphere: this fragment is not part of the layer.
		// DISCARD, never "write transparent black" — see the foot.
		if (distances.y < 0.0) {
			discard;
		}
		vec3 pos = ray_origin + (ray_dir * distances.y);
		dir_from_sph = normalize(pos);
	}

	// Spherical coordinates. GLSL's atan(y, x) is HLSL's atan2(y, x).
	float lon = atan(dir_from_sph.x, -dir_from_sph.z) / (2.0 * PI) + 0.5;
	float lat = acos(dir_from_sph.y) / PI;

	float chan = central_horizontal_angle / (PI * 2.0);
	// Normalize [0, 2pi] to [0, 1]
	float uhan = 0.5 + chan / 2.0;
	float lhan = 0.5 - chan / 2.0;
	// Normalize [-pi/2, pi/2] to [0, 1]
	float uvan = upper_vertical_angle / PI + 0.5;
	float lvan = lower_vertical_angle / PI + 0.5;

	if (lat < uvan && lat > lvan && lon < uhan && lon > lhan) {
		// Map the configured display region to the whole texture. No Y flip:
		// sample_point.y == 0 is the TOP of the section, and a Vulkan image's
		// v == 0 is its top row — the HLSL's reading, not GL's.
		vec2 ll_offset = vec2(lhan, lvan);
		vec2 ll_extent = vec2(uhan - lhan, uvan - lvan);
		vec2 sample_point = (vec2(lon, lat) - ll_offset) / ll_extent;
		vec2 uv_sub = sample_point * pc.src_rect.zw + pc.src_rect.xy;
		out_color = DXR_EQ2_SAMPLE(uv_sub) * pc.color_scale + pc.color_bias;
	} else {
		// OUTSIDE the layer's angular extent. An unflagged layer resolves to
		// OPAQUE_COVER, whose pipeline has blending DISABLED, so writing
		// (0,0,0,0) here would OVERWRITE the tile everywhere the section does
		// not reach (CTS Equirect2 5/6). `discard` is correct in every mode.
		discard;
	}
}
