// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  HLSL of the reference view synthesis (ADR-053 S5), compiled at
 *         create with D3DCompile like the service's other inline shaders.
 *
 * A line-for-line port of displayxr-web-lift `js/lift/shaders/dibr.frag.glsl.js`
 * and `dibr-upsample.frag.glsl.js` (GLSL ES 3.00). The only systematic change
 * is orientation: every D3D texture here has row 0 = image TOP, so the
 * GLSL's `1.0 - v` flips (its colour was uploaded with UNPACK_FLIP_Y) move to
 * the one place the math works "v up" — the main pass — and nowhere else.
 *
 * Intermediate depth rasters hold the UNCLAMPED normalised nearness
 * u = (N - N_far) / (N_near - N_far): an increasing affine map of the JS's raw
 * disparity with lo = 0, hi = 1, so bilinear sampling, the JBU's convex
 * combinations and the min / max filters commute with it and every JS
 * threshold applies unchanged (snapTau * (hi - lo) == snapTau).
 *
 * @ingroup comp_d3d11_service
 */

#pragma once

//! Full-viewport triangle (dibr.vert.glsl.js); the pixel shaders read SV_Position.
static const char k_viewsyn_vs[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

/*!
 * Depth res: raw sample -> decoded -> nearness -> unclamped normalised u,
 * written to r, g and b (the snap's cores start equal to the value).
 */
static const char k_viewsyn_prep_ps[] = R"(
cbuffer Prep : register(b0)
{
	float value_scale;
	float value_shift;
	float near_lo;
	float inv_span;
	uint kind;          // XRT_CONV_DEPTH_*
	uint has_invalid;
	float invalid_sample;
	float pad0;
};
Texture2D<float4> t_depth : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
	float s = t_depth.Load(int3(int2(pos.xy), 0)).r;
	if (has_invalid != 0 && s == invalid_sample)
		return float4(0.0, 0.0, 0.0, 1.0);
	float d = value_scale * s + value_shift;
	float n = d;
	if (kind == 1u)
		n = d > 0.0 ? 1.0 / d : 0.0;   // METRIC: 1/z (live-dibr.js toDisparity)
	else if (kind == 5u)
		n = -d;                        // AFFINE depth: larger = farther
	float u = (n - near_lo) * inv_span;
	return float4(u, u, u, 1.0);
}
)";

/*!
 * One direction of the separable filters (live-dibr.js dilateMax / erodeMin /
 * snapPack, and MAX_FRAG): r = max over rad_r, g = min over rad_c, b = max
 * over rad_c, each window clamped to the image. `seed` = take g / b from r.
 */
static const char k_viewsyn_filter_ps[] = R"(
cbuffer Filter : register(b0)
{
	int2 dir;
	int rad_r;
	int rad_c;
	int seed;
	int3 pad0;
};
Texture2D<float4> t_src : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target
{
	int2 p = int2(pos.xy);
	uint w, h;
	t_src.GetDimensions(w, h);
	int2 hi = int2(w, h) - 1;
	float4 c = t_src.Load(int3(p, 0));
	float r = c.r;
	float g = seed != 0 ? c.r : c.g;
	float b = seed != 0 ? c.r : c.b;
	[loop] for (int k = 1; k <= 16; k++) {
		if (k > rad_r && k > rad_c) break;
		float4 s0 = t_src.Load(int3(clamp(p + dir * k, int2(0, 0), hi), 0));
		float4 s1 = t_src.Load(int3(clamp(p - dir * k, int2(0, 0), hi), 0));
		if (k <= rad_r) r = max(r, max(s0.r, s1.r));
		if (k <= rad_c) {
			g = min(g, seed != 0 ? min(s0.r, s1.r) : min(s0.g, s1.g));
			b = max(b, seed != 0 ? max(s0.r, s1.r) : max(s0.b, s1.b));
		}
	}
	return float4(r, g, b, 1.0);
}
)";

//! GUIDE_FRAG: the colour box-filtered to depth resolution (4 bilinear taps).
static const char k_viewsyn_guide_ps[] = R"(
cbuffer Guide : register(b0)
{
	float2 out_res;
	float2 pad0;
};
Texture2D<float4> t_color : register(t0);
SamplerState s_lin : register(s0);
float4 main(float4 pos : SV_Position) : SV_Target
{
	float2 uv = pos.xy / out_res;
	float2 h = 0.25 / out_res;
	float3 c = t_color.SampleLevel(s_lin, uv + float2(-h.x, -h.y), 0).rgb
	         + t_color.SampleLevel(s_lin, uv + float2( h.x, -h.y), 0).rgb
	         + t_color.SampleLevel(s_lin, uv + float2(-h.x,  h.y), 0).rgb
	         + t_color.SampleLevel(s_lin, uv + float2( h.x,  h.y), 0).rgb;
	return float4(0.25 * c, 1.0);
}
)";

//! UPSAMPLE_FRAG: joint-bilateral upsample (Kopf 4x4) + EDGE SNAP.
static const char k_viewsyn_upsample_ps[] = R"(
cbuffer Up : register(b0)
{
	float2 out_res;
	float sigma_s;   // low-res px
	float sigma_r;   // RGB 0..1
	int snap;
	int snap_r;      // <= 12
	float snap_tau;  // normalised units
	float pad0;
};
Texture2D<float4> t_dlo : register(t0);    // r = u, g = min-filtered, b = max-filtered; row 0 = top
Texture2D<float4> t_guide : register(t1);  // depth-res colour
Texture2D<float4> t_color : register(t2);  // row 0 = top
SamplerState s_lin : register(s0);
float4 main(float4 pos : SV_Position) : SV_Target
{
	uint lw, lh;
	t_dlo.GetDimensions(lw, lh);
	float2 lo = float2(lw, lh);
	int2 loI = int2(lw, lh) - 1;
	float2 xy = pos.xy / out_res;                 // 0..1, y from the TOP
	float3 c0 = t_color.SampleLevel(s_lin, xy, 0).rgb;
	float2 pl = xy * lo - 0.5;
	float2 b = floor(pl);
	float inv2s = 1.0 / (2.0 * sigma_s * sigma_s);
	float inv2r = 1.0 / (2.0 * sigma_r * sigma_r);
	float sw = 0.0, sd = 0.0;
	[unroll] for (int j = -1; j <= 2; j++) {
		[unroll] for (int i = -1; i <= 2; i++) {
			int2 s = clamp(int2(b) + int2(i, j), int2(0, 0), loI);
			float2 dd = float2(s) - pl;
			float ws = exp(-dot(dd, dd) * inv2s);
			float3 dc = t_guide.Load(int3(s, 0)).rgb - c0;
			float w = ws * (exp(-dot(dc, dc) * inv2r) + 1e-3);
			sw += w;
			sd += w * t_dlo.Load(int3(s, 0)).r;
		}
	}
	float dj = sd / sw;
	float4 o = float4(dj, 0.0, 0.0, 1.0);
	if (snap == 0) return o;

	float3 here = t_dlo.SampleLevel(s_lin, (pl + 0.5) / lo, 0).rgb;
	float dmax = here.b, dmin = here.g;
	float rng = dmax - dmin;
	float eW = smoothstep(snap_tau, 2.0 * snap_tau, rng);
	if (eW <= 0.0) return o;
	float th = 0.25 * rng;
	int2 ci = int2(floor(pl + 0.5));
	float R = float(snap_r);
	float inv2c = 1.0 / (2.0 * 0.36 * R * R);

	float3 sf = float3(0.0, 0.0, 0.0), sb = float3(0.0, 0.0, 0.0);
	float nf = 0.0, nb = 0.0, df = 0.0, db = 0.0;
	[loop] for (int jj = -12; jj <= 12; jj++) {
		if (abs(jj) > snap_r) continue;
		[loop] for (int ii = -12; ii <= 12; ii++) {
			if (abs(ii) > snap_r) continue;
			int2 s = clamp(ci + int2(ii, jj), int2(0, 0), loI);
			float2 dd = float2(ci + int2(ii, jj)) - pl;
			float w = exp(-dot(dd, dd) * inv2c) * saturate(R - length(dd));
			float3 t = t_dlo.Load(int3(s, 0)).rgb;
			float3 g = t_guide.Load(int3(s, 0)).rgb;
			float wf = w * smoothstep(dmax - 2.0 * th, dmax - 0.5 * th, t.g);
			float wb = w * (1.0 - smoothstep(dmin + 0.5 * th, dmin + 2.0 * th, t.b));
			sf += wf * g; df += wf * t.r; nf += wf;
			sb += wb * g; db += wb * t.r; nb += wb;
		}
	}
	float conf = smoothstep(0.05, 0.4, min(nf, nb));
	if (conf <= 0.0) return o;
	float3 cf = sf / nf, cb = sb / nb;
	float3 ab = cf - cb;
	float l2 = dot(ab, ab);
	float sep = smoothstep(0.005, 0.02, l2);
	if (sep <= 0.0) return o;
	float aC = saturate(dot(c0 - cb, ab) / l2);
	float aM = saturate((dj - dmin) / rng);
	float a = smoothstep(0.3, 0.7, lerp(aC, aM, 0.2));
	float ds = lerp(db / nb, df / nf, a);
	return float4(lerp(dj, ds, eW * sep * conf), 0.0, 0.0, 1.0);
}
)";

/*!
 * dibr.frag.glsl.js: one draw per view. Coordinates in window HEIGHTS, origin
 * at the rect centre, +y up, +z toward the viewer (u_conv_viewsyn.h).
 */
static const char k_viewsyn_main_ps[] = R"(
cbuffer Main : register(b0)
{
	float2 vp_origin;   // this view's viewport in the target, px
	float2 vp_size;
	float2 disp_res;    // DEPTH-grid size: sizes the disocclusion probe
	float conv;         // n_conv
	float qscale;
	float taper;
	float aspect;
	float rear_k;       // ADR-040 rear cap: scale on q < 0
	int steps;
	float3 eye;
	int debug;
	float3 cam;
	float pad0;
};
Texture2D<float4> t_color : register(t0);  // row 0 = top
Texture2D<float4> t_disp : register(t1);   // r = unclamped normalised nearness, row 0 = top
SamplerState s_lin : register(s0);

float qAt(float2 uvs)
{
	float n = saturate(t_disp.SampleLevel(s_lin, float2(uvs.x, 1.0 - uvs.y), 0).r);
	float2 b = min(uvs, 1.0 - uvs);
	float t = taper > 0.0 ? smoothstep(0.0, taper, min(b.x, b.y / aspect)) : 1.0;
	float q = qscale * (n - conv) * t;
	return q < 0.0 ? q * rear_k : q;
}

float2 srcUv(float2 pp, float q)
{
	float z = q * cam.z / (1.0 + q);
	float2 r = eye.xy + (pp - eye.xy) * ((eye.z - z) / eye.z);
	float2 xs = cam.xy + (r - cam.xy) * (1.0 + q);
	return float2(xs.x / aspect + 0.5, xs.y + 0.5);
}

float4 colorAt(float2 uv)
{
	return t_color.SampleLevel(s_lin, float2(uv.x, 1.0 - uv.y), 0);
}

float4 main(float4 pos : SV_Position) : SV_Target
{
	float2 vUv = float2((pos.x - vp_origin.x) / vp_size.x, 1.0 - (pos.y - vp_origin.y) / vp_size.y);
	if (debug == 1) {
		float n = saturate(t_disp.SampleLevel(s_lin, float2(vUv.x, 1.0 - vUv.y), 0).r);
		return float4(n, n, n, 1.0);
	}
	float2 pp = float2((vUv.x - 0.5) * aspect, vUv.y - 0.5);
	float qNear = qscale * (1.0 - conv);
	float qFar = qscale * (0.0 - conv);
	if (qNear < 0.0) qNear *= rear_k;
	if (qFar < 0.0) qFar *= rear_k;
	qNear = max(qNear, 0.0);
	qFar = min(qFar, 0.0);

	// ---- coarse march, near -> far: first step where the surface is in front of the ray ----
	float dq = (qNear - qFar) / float(steps);
	float q = qNear;
	float qPrev = qNear;
	bool hit = false;
	[loop] for (int i = 0; i <= 64; i++) {
		if (i > steps) break;
		q = qNear - dq * float(i);
		if (qAt(srcUv(pp, q)) >= q) { hit = true; break; }
		qPrev = q;
	}
	if (!hit) q = qFar;
	// ---- bisection refine between qPrev (ray in front of surface) and q (behind/on) ----
	if (hit && q != qPrev) {
		float a = qPrev, bb = q;
		[unroll] for (int k = 0; k < 6; k++) {
			float m = 0.5 * (a + bb);
			if (qAt(srcUv(pp, m)) >= m) bb = m; else a = m;
		}
		q = bb;
	}
	float2 uv = srcUv(pp, q);

	// ---- disocclusion: background-side gather (iw3 shift_fill equivalent) ----
	float2 off = eye.xy - cam.xy;
	float2 dir = length(off) > 1e-6 ? normalize(off) : float2(1.0, 0.0);
	float2 texel = float2(dir.x / disp_res.x, dir.y / disp_res.y);
	float2 dUv = texel * 2.0;
	float qa = qAt(uv - dUv), qb = qAt(uv + dUv);
	float jump = abs(qa - qb);
	float edge = smoothstep(0.15, 0.5, jump / max(abs(qscale), 1e-6));
	edge *= saturate(2.0 * (max(qa, qb) - q) / max(jump, 1e-6));
	float4 col = colorAt(uv);
	float hole = length(off) * jump;
	if (edge > 0.0 && hole > 0.0) {
		float side = qa < qb ? -1.0 : 1.0;
		float2 step1 = float2(dir.x / aspect, dir.y) * hole * side;
		float2 ub = uv + step1 + dUv * side;
		float4 bg = 0.5 * colorAt(ub) + 0.25 * colorAt(ub + step1 * 0.5) + 0.25 * colorAt(ub - step1 * 0.25);
		col = lerp(col, bg, edge * saturate(hole * disp_res.y * 0.5));
	}
	return float4(col.rgb, 1.0);
}
)";
