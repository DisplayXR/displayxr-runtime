// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 S5 — the Vulkan reference view synthesis (vk_conv_viewsyn)
 *         against a CPU golden of the same algorithm, plus the D3D11 stage
 *         test's geometric oracles.
 *
 * The scene is tests_conv_viewsyn_d3d11.cpp's: a colour image that ENCODES its
 * own coordinates (R = source u, G = source v from the bottom, B = 1 on a near
 * square), and an affine-disparity depth with the square at 1 and the ground
 * at 0, on a 4x coarser grid.
 *
 * Two kinds of check:
 *
 *  - GOLDEN: every pass of the stage (prep, separable filters, guide, joint
 *    bilateral upsample + edge snap, the per-view march + disocclusion fill) is
 *    re-implemented below on the CPU, line for line from the GLSL (itself a
 *    line-for-line port of the D3D11 HLSL), with the same intermediate
 *    precision (RGBA16F rasters, an RGBA8 guide, an RGBA16F view). The GPU
 *    output must match it within a tolerance: the GPU's bilinear filter uses
 *    a fixed-point sub-texel weight and its exp() is approximate, which moves
 *    a few silhouette pixels by one march step, so the check is statistical
 *    (mean error, and the share of pixels off by more than 4/255).
 *  - ORACLES (ported from the D3D11 test): coverage, plateau parallax equal to
 *    u_conv_viewsyn_march, the occluding edge where the closed form puts it,
 *    monotonic shift across views, a view at the source camera = identity.
 *
 * The D3D11 stage itself cannot run here; both GPU stages are pinned to the
 * same u_conv_viewsyn math and the same oracles, and this golden pins the
 * passes the oracles cannot see (the JBU / snap / dilation / hole fill).
 *
 * SKIPs when there is no Vulkan device.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "vktest_conv_bundle.hpp"

#include "vk_conv/vk_conv_viewsyn.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace vktest_conv;

namespace {

constexpr uint32_t kW = 512, kH = 288;                             // colour = one output view
constexpr uint32_t kDW = 128, kDH = 72;                            // depth grid (4 px per depth texel)
constexpr uint32_t kSqX0 = 48, kSqX1 = 80, kSqY0 = 24, kSqY1 = 48; // depth texels, row 0 = top
constexpr float kRectW = 0.32f, kRectH = 0.18f;


/*
 *
 * The CPU golden.
 *
 */

//! An RGBA float raster with the GPU's sampling rules (clamp to edge).
struct Tex
{
	uint32_t w = 0, h = 0;
	std::vector<float> px; // rgba

	void
	init(uint32_t nw, uint32_t nh)
	{
		w = nw;
		h = nh;
		px.assign((size_t)w * h * 4, 0.0f);
	}
	float *
	at(int x, int y)
	{
		x = std::clamp(x, 0, (int)w - 1);
		y = std::clamp(y, 0, (int)h - 1);
		return &px[((size_t)y * w + x) * 4];
	}
	const float *
	at(int x, int y) const
	{
		x = std::clamp(x, 0, (int)w - 1);
		y = std::clamp(y, 0, (int)h - 1);
		return &px[((size_t)y * w + x) * 4];
	}
	//! textureLod(…, 0) with a LINEAR / CLAMP_TO_EDGE sampler.
	void
	sample(float u, float v, float out[4]) const
	{
		const float x = u * (float)w - 0.5f, y = v * (float)h - 0.5f;
		const float x0 = std::floor(x), y0 = std::floor(y);
		const float fx = x - x0, fy = y - y0;
		const float *a = at((int)x0, (int)y0), *b = at((int)x0 + 1, (int)y0);
		const float *c = at((int)x0, (int)y0 + 1), *d = at((int)x0 + 1, (int)y0 + 1);
		for (int k = 0; k < 4; k++) {
			const float top = a[k] + (b[k] - a[k]) * fx;
			const float bot = c[k] + (d[k] - c[k]) * fx;
			out[k] = top + (bot - top) * fy;
		}
	}
	void
	quantize_half_all()
	{
		for (float &f : px) {
			f = quantize_half(f);
		}
	}
	void
	quantize_unorm8_all()
	{
		for (float &f : px) {
			f = std::round(std::clamp(f, 0.0f, 1.0f) * 255.0f) / 255.0f;
		}
	}
};

float
clamp01(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

float
smoothstepf(float e0, float e1, float x)
{
	const float t = clamp01((x - e0) / (e1 - e0));
	return t * t * (3.0f - 2.0f * t);
}

//! conv_viewsyn_filter.comp, one direction, into @p dst.
void
cpu_filter(const Tex &src, Tex &dst, int dx, int dy, int rad_r, int rad_c, bool seed)
{
	dst.init(src.w, src.h);
	for (int y = 0; y < (int)src.h; y++) {
		for (int x = 0; x < (int)src.w; x++) {
			const float *c = src.at(x, y);
			float r = c[0];
			float g = seed ? c[0] : c[1];
			float b = seed ? c[0] : c[2];
			for (int k = 1; k <= 16; k++) {
				if (k > rad_r && k > rad_c) {
					break;
				}
				const float *s0 = src.at(x + dx * k, y + dy * k);
				const float *s1 = src.at(x - dx * k, y - dy * k);
				if (k <= rad_r) {
					r = std::max(r, std::max(s0[0], s1[0]));
				}
				if (k <= rad_c) {
					g = std::min(g, seed ? std::min(s0[0], s1[0]) : std::min(s0[1], s1[1]));
					b = std::max(b, seed ? std::max(s0[0], s1[0]) : std::max(s0[2], s1[2]));
				}
			}
			float *o = dst.at(x, y);
			o[0] = r;
			o[1] = g;
			o[2] = b;
			o[3] = 1.0f;
		}
	}
	dst.quantize_half_all();
}

void
cpu_filter2(Tex &src, Tex &tmp, int rad_r, int rad_c, bool seed)
{
	cpu_filter(src, tmp, 1, 0, rad_r, rad_c, seed);
	cpu_filter(tmp, src, 0, 1, rad_r, rad_c, false);
}

/*!
 * The whole stage on the CPU: @p out[i] = view i (RGBA, kW x kH, RGBA16F
 * precision). Mirrors vk_conv_viewsyn_record + the five shaders.
 */
void
cpu_viewsyn(const Tex &color,
            const std::vector<float> &depth,
            uint32_t dw,
            uint32_t dh,
            const u_conv_viewsyn_params &p,
            uint32_t vw,
            uint32_t vh,
            std::vector<Tex> &out)
{
	u_conv_viewsyn_frame f;
	REQUIRE(u_conv_viewsyn_resolve(&p, &f));

	// 1. prep.
	Tex dlo, tmp;
	dlo.init(dw, dh);
	const float vscale = p.depth.value_scale == 0.0f ? 1.0f : p.depth.value_scale;
	const float near_lo = (float)f.near_lo;
	const float inv_span = (float)(1.0 / (f.near_hi - f.near_lo));
	const bool has_invalid = !std::isnan(p.depth.invalid_sample);
	for (uint32_t y = 0; y < dh; y++) {
		for (uint32_t x = 0; x < dw; x++) {
			const float s = depth[(size_t)y * dw + x];
			float *o = dlo.at((int)x, (int)y);
			if (has_invalid && s == p.depth.invalid_sample) {
				o[0] = o[1] = o[2] = 0.0f;
				o[3] = 1.0f;
				continue;
			}
			const float d = vscale * s + p.depth.value_shift;
			float n = d;
			if (p.depth.kind == XRT_CONV_DEPTH_METRIC) {
				n = d > 0.0f ? 1.0f / d : 0.0f;
			} else if (p.depth.kind == XRT_CONV_DEPTH_AFFINE) {
				n = -d;
			}
			const float u = (n - near_lo) * inv_span;
			o[0] = o[1] = o[2] = u;
			o[3] = 1.0f;
		}
	}
	dlo.quantize_half_all();

	// 2-3. dilation, snap cores.
	const uint32_t dilate = std::min(p.dilate, 16u);
	const bool snap = p.upsample && p.snap;
	uint32_t core = 0, radius = 0;
	if (snap) {
		u_conv_viewsyn_snap_radii(p.snap_core, p.snap_radius, dw, &core, &radius);
		core = std::min(core, 16u);
	}
	if (dilate > 0) {
		cpu_filter2(dlo, tmp, (int)dilate, 0, false);
	}
	if (core > 0) {
		cpu_filter2(dlo, tmp, 0, (int)core, true);
	} else if (dilate > 0) {
		cpu_filter2(dlo, tmp, 0, 0, true);
	}
	const Tex *disp = &dlo;

	Tex guide, up_a, up_b;
	if (p.upsample) {
		// 4. guide.
		guide.init(dw, dh);
		for (uint32_t y = 0; y < dh; y++) {
			for (uint32_t x = 0; x < dw; x++) {
				const float u = (x + 0.5f) / dw, v = (y + 0.5f) / dh;
				const float hx = 0.25f / dw, hy = 0.25f / dh;
				float a[4], b[4], c[4], d[4];
				color.sample(u - hx, v - hy, a);
				color.sample(u + hx, v - hy, b);
				color.sample(u - hx, v + hy, c);
				color.sample(u + hx, v + hy, d);
				float *o = guide.at((int)x, (int)y);
				for (int k = 0; k < 3; k++) {
					o[k] = 0.25f * (a[k] + b[k] + c[k] + d[k]);
				}
				o[3] = 1.0f;
			}
		}
		guide.quantize_unorm8_all();

		// 5. JBU + edge snap.
		uint32_t uw, uh;
		u_conv_viewsyn_upsample_size(color.w, color.h, p.upsample_max > 0 ? p.upsample_max : 1, &uw, &uh);
		up_a.init(uw, uh);
		const float sigma_s = std::max(0.1f, p.upsample_sigma_s > 0.0f ? p.upsample_sigma_s : 1.0f);
		const float sigma_r = std::max(0.005f, p.upsample_sigma_r > 0.0f ? p.upsample_sigma_r : 0.1f);
		const float snap_tau = std::max(1e-6f, p.snap_tau);
		const float lo[2] = {(float)dw, (float)dh};
		for (uint32_t y = 0; y < uh; y++) {
			for (uint32_t x = 0; x < uw; x++) {
				const float xy[2] = {(x + 0.5f) / uw, (y + 0.5f) / uh};
				float c0[4];
				color.sample(xy[0], xy[1], c0);
				const float pl[2] = {xy[0] * lo[0] - 0.5f, xy[1] * lo[1] - 0.5f};
				const float bx = std::floor(pl[0]), by = std::floor(pl[1]);
				const float inv2s = 1.0f / (2.0f * sigma_s * sigma_s);
				const float inv2r = 1.0f / (2.0f * sigma_r * sigma_r);
				float sw = 0.0f, sd = 0.0f;
				for (int j = -1; j <= 2; j++) {
					for (int i = -1; i <= 2; i++) {
						const int sx = std::clamp((int)bx + i, 0, (int)dw - 1);
						const int sy = std::clamp((int)by + j, 0, (int)dh - 1);
						const float ddx = sx - pl[0], ddy = sy - pl[1];
						const float ws = std::exp(-(ddx * ddx + ddy * ddy) * inv2s);
						const float *g = guide.at(sx, sy);
						const float dcx = g[0] - c0[0], dcy = g[1] - c0[1], dcz = g[2] - c0[2];
						const float w =
						    ws *
						    (std::exp(-(dcx * dcx + dcy * dcy + dcz * dcz) * inv2r) + 1e-3f);
						sw += w;
						sd += w * dlo.at(sx, sy)[0];
					}
				}
				const float dj = sd / sw;
				float result = dj;
				if (snap) {
					float here[4];
					dlo.sample((pl[0] + 0.5f) / lo[0], (pl[1] + 0.5f) / lo[1], here);
					const float dmax = here[2], dmin = here[1];
					const float rng = dmax - dmin;
					const float eW = smoothstepf(snap_tau, 2.0f * snap_tau, rng);
					if (eW > 0.0f) {
						const float th = 0.25f * rng;
						const int cix = (int)std::floor(pl[0] + 0.5f),
						          ciy = (int)std::floor(pl[1] + 0.5f);
						const float R = (float)radius;
						const float inv2c = 1.0f / (2.0f * 0.36f * R * R);
						float sf[3] = {0, 0, 0}, sb[3] = {0, 0, 0};
						float nf = 0, nb = 0, df = 0, db = 0;
						for (int jj = -12; jj <= 12; jj++) {
							if (std::abs(jj) > (int)radius) {
								continue;
							}
							for (int ii = -12; ii <= 12; ii++) {
								if (std::abs(ii) > (int)radius) {
									continue;
								}
								const int sx = std::clamp(cix + ii, 0, (int)dw - 1);
								const int sy = std::clamp(ciy + jj, 0, (int)dh - 1);
								const float ddx = (float)(cix + ii) - pl[0];
								const float ddy = (float)(ciy + jj) - pl[1];
								const float len = std::sqrt(ddx * ddx + ddy * ddy);
								const float w =
								    std::exp(-(ddx * ddx + ddy * ddy) * inv2c) *
								    clamp01(R - len);
								const float *t = dlo.at(sx, sy);
								const float *g = guide.at(sx, sy);
								const float wf =
								    w * smoothstepf(dmax - 2.0f * th, dmax - 0.5f * th,
								                    t[1]);
								const float wb =
								    w * (1.0f - smoothstepf(dmin + 0.5f * th,
								                            dmin + 2.0f * th, t[2]));
								for (int k = 0; k < 3; k++) {
									sf[k] += wf * g[k];
									sb[k] += wb * g[k];
								}
								df += wf * t[0];
								nf += wf;
								db += wb * t[0];
								nb += wb;
							}
						}
						const float conf = smoothstepf(0.05f, 0.4f, std::min(nf, nb));
						if (conf > 0.0f) {
							float ab[3], l2 = 0.0f, dot_c = 0.0f;
							for (int k = 0; k < 3; k++) {
								ab[k] = sf[k] / nf - sb[k] / nb;
								l2 += ab[k] * ab[k];
							}
							const float sep = smoothstepf(0.005f, 0.02f, l2);
							if (sep > 0.0f) {
								for (int k = 0; k < 3; k++) {
									dot_c += (c0[k] - sb[k] / nb) * ab[k];
								}
								const float aC = clamp01(dot_c / l2);
								const float aM = clamp01((dj - dmin) / rng);
								const float a =
								    smoothstepf(0.3f, 0.7f, aC + (aM - aC) * 0.2f);
								const float ds = db / nb + (df / nf - db / nb) * a;
								result = dj + (ds - dj) * (eW * sep * conf);
							}
						}
					}
				}
				float *o = up_a.at((int)x, (int)y);
				o[0] = result;
				o[1] = 0.0f;
				o[2] = 0.0f;
				o[3] = 1.0f;
			}
		}
		up_a.quantize_half_all();
		const uint32_t dilate_hi = std::min(p.dilate_hi, 8u);
		if (dilate_hi > 0) {
			cpu_filter2(up_a, up_b, (int)dilate_hi, 0, false);
		}
		disp = &up_a;
	}

	// 7. the march, per view.
	const float conv = (float)f.conv_n, taper = (float)f.taper, aspect = (float)f.aspect;
	const float rear_k = (float)f.rear_k;
	const float cam[3] = {(float)f.cam.x, (float)f.cam.y, (float)f.cam.z};
	out.assign(f.view_count, Tex());
	for (uint32_t vi = 0; vi < f.view_count; vi++) {
		Tex &dst = out[vi];
		dst.init(vw, vh);
		const float qscale = (float)f.views[vi].q_scale;
		const float eye[3] = {(float)f.views[vi].eye.x, (float)f.views[vi].eye.y, (float)f.views[vi].eye.z};
		auto qAt = [&](float ux, float uy) {
			float s[4];
			disp->sample(ux, 1.0f - uy, s);
			const float n = clamp01(s[0]);
			const float bx = std::min(ux, 1.0f - ux), by = std::min(uy, 1.0f - uy);
			const float t = taper > 0.0f ? smoothstepf(0.0f, taper, std::min(bx, by / aspect)) : 1.0f;
			const float q = qscale * (n - conv) * t;
			return q < 0.0f ? q * rear_k : q;
		};
		auto srcUv = [&](float ppx, float ppy, float q, float o[2]) {
			const float z = q * cam[2] / (1.0f + q);
			const float k = (eye[2] - z) / eye[2];
			const float rx = eye[0] + (ppx - eye[0]) * k, ry = eye[1] + (ppy - eye[1]) * k;
			const float xs = cam[0] + (rx - cam[0]) * (1.0f + q), ys = cam[1] + (ry - cam[1]) * (1.0f + q);
			o[0] = xs / aspect + 0.5f;
			o[1] = ys + 0.5f;
		};
		auto colorAt = [&](float ux, float uy, float o[4]) { color.sample(ux, 1.0f - uy, o); };
		for (uint32_t y = 0; y < vh; y++) {
			for (uint32_t x = 0; x < vw; x++) {
				const float posx = x + 0.5f, posy = y + 0.5f;
				const float vux = posx / vw, vuy = 1.0f - posy / vh;
				float *o = dst.at((int)x, (int)y);
				if (p.debug == 1) {
					float s[4];
					disp->sample(vux, 1.0f - vuy, s);
					o[0] = o[1] = o[2] = clamp01(s[0]);
					o[3] = 1.0f;
					continue;
				}
				const float ppx = (vux - 0.5f) * aspect, ppy = vuy - 0.5f;
				float qNear = qscale * (1.0f - conv), qFar = qscale * (0.0f - conv);
				if (qNear < 0.0f) {
					qNear *= rear_k;
				}
				if (qFar < 0.0f) {
					qFar *= rear_k;
				}
				qNear = std::max(qNear, 0.0f);
				qFar = std::min(qFar, 0.0f);
				const float dq = (qNear - qFar) / (float)f.steps;
				float q = qNear, qPrev = qNear;
				bool hit = false;
				float uv[2];
				for (int i = 0; i <= 64; i++) {
					if (i > (int)f.steps) {
						break;
					}
					q = qNear - dq * (float)i;
					srcUv(ppx, ppy, q, uv);
					if (qAt(uv[0], uv[1]) >= q) {
						hit = true;
						break;
					}
					qPrev = q;
				}
				if (!hit) {
					q = qFar;
				}
				if (hit && q != qPrev) {
					float a = qPrev, bb = q;
					for (int k = 0; k < 6; k++) {
						const float m = 0.5f * (a + bb);
						srcUv(ppx, ppy, m, uv);
						if (qAt(uv[0], uv[1]) >= m) {
							bb = m;
						} else {
							a = m;
						}
					}
					q = bb;
				}
				srcUv(ppx, ppy, q, uv);

				const float offx = eye[0] - cam[0], offy = eye[1] - cam[1];
				const float offl = std::sqrt(offx * offx + offy * offy);
				const float dirx = offl > 1e-6f ? offx / offl : 1.0f,
				            diry = offl > 1e-6f ? offy / offl : 0.0f;
				const float dUx = dirx / (float)dw * 2.0f, dUy = diry / (float)dh * 2.0f;
				const float qa = qAt(uv[0] - dUx, uv[1] - dUy), qb = qAt(uv[0] + dUx, uv[1] + dUy);
				const float jump = std::fabs(qa - qb);
				float edge = smoothstepf(0.15f, 0.5f, jump / std::max(std::fabs(qscale), 1e-6f));
				edge *= clamp01(2.0f * (std::max(qa, qb) - q) / std::max(jump, 1e-6f));
				float col[4];
				colorAt(uv[0], uv[1], col);
				const float hole = offl * jump;
				if (edge > 0.0f && hole > 0.0f) {
					const float side = qa < qb ? -1.0f : 1.0f;
					const float s1x = dirx / aspect * hole * side, s1y = diry * hole * side;
					const float ubx = uv[0] + s1x + dUx * side, uby = uv[1] + s1y + dUy * side;
					float c1[4], c2[4], c3[4];
					colorAt(ubx, uby, c1);
					colorAt(ubx + s1x * 0.5f, uby + s1y * 0.5f, c2);
					colorAt(ubx - s1x * 0.25f, uby - s1y * 0.25f, c3);
					const float wgt = edge * clamp01(hole * (float)dh * 0.5f);
					for (int k = 0; k < 4; k++) {
						const float bg = 0.5f * c1[k] + 0.25f * c2[k] + 0.25f * c3[k];
						col[k] = col[k] + (bg - col[k]) * wgt;
					}
				}
				o[0] = col[0];
				o[1] = col[1];
				o[2] = col[2];
				o[3] = 1.0f;
			}
		}
		dst.quantize_half_all();
	}
}


/*
 *
 * The GPU side.
 *
 */

struct Fixture
{
	unique_bundle vk;
	struct vk_cmd_pool pool = {};
	bool pool_ok = false;
	Image color, depth;
	Tex color_cpu;
	std::vector<float> depth_cpu;
	vk_conv_viewsyn *vs = nullptr;

	~Fixture()
	{
		vk_conv_viewsyn_destroy(&vs);
		if (pool_ok) {
			vk_cmd_pool_destroy(vk.get(), &pool);
		}
	}
};

//! The scene: colour (RGBA16F) and depth; @p metric = metres (square 1 m, ground 2 m).
bool
make_fixture(Fixture &f, bool metric = false)
{
	f.vk = make_bundle();
	if (!f.vk) {
		return false;
	}
	struct vk_bundle *vk = f.vk.get();
	REQUIRE(vk_cmd_pool_init(vk, &f.pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
	f.pool_ok = true;

	f.color_cpu.init(kW, kH);
	std::vector<uint16_t> c((size_t)kW * kH * 4);
	for (uint32_t y = 0; y < kH; y++) {
		for (uint32_t x = 0; x < kW; x++) {
			const bool in_sq = x >= kSqX0 * 4 && x < kSqX1 * 4 && y >= kSqY0 * 4 && y < kSqY1 * 4;
			float *p = f.color_cpu.at((int)x, (int)y);
			p[0] = (x + 0.5f) / kW;
			p[1] = 1.0f - (y + 0.5f) / kH;
			p[2] = in_sq ? 1.0f : 0.0f;
			p[3] = 1.0f;
			for (int k = 0; k < 4; k++) {
				c[((size_t)y * kW + x) * 4 + k] = to_half_bits(p[k]);
				p[k] = quantize_half(p[k]);
			}
		}
	}
	REQUIRE(make_image(vk, f.color, kW, kH, 1, VK_FORMAT_R16G16B16A16_SFLOAT));
	REQUIRE(upload(vk, &f.pool, f.color, c.data(), c.size() * 2));

	f.depth_cpu.assign((size_t)kDW * kDH, metric ? 2.0f : 0.0f);
	for (uint32_t y = kSqY0; y < kSqY1; y++) {
		for (uint32_t x = kSqX0; x < kSqX1; x++) {
			f.depth_cpu[(size_t)y * kDW + x] = 1.0f;
		}
	}
	REQUIRE(make_image(vk, f.depth, kDW, kDH, 1, VK_FORMAT_R32_SFLOAT));
	REQUIRE(upload(vk, &f.pool, f.depth, f.depth_cpu.data(), f.depth_cpu.size() * 4));

	f.vs = vk_conv_viewsyn_create(vk);
	REQUIRE(f.vs != nullptr);
	return true;
}

struct Px
{
	double r, g, b;
	uint16_t raw[4];
};

//! Render into a fresh RGBA16F target (magenta-cleared) and read back per view.
bool
render(Fixture &f,
       const u_conv_viewsyn_params &p,
       uint32_t layout,
       std::vector<std::vector<Px>> &views,
       bool new_content = true)
{
	struct vk_bundle *vk = f.vk.get();
	const uint32_t n = p.view_count;
	const uint32_t tw = layout == XRT_CONV_VIEWS_ROW ? kW * n : kW;
	const uint32_t tl = layout == XRT_CONV_VIEWS_ROW ? 1 : n;
	Image t;
	REQUIRE(make_image(vk, t, tw, kH, tl, VK_FORMAT_R16G16B16A16_SFLOAT));
	bool rec = false;
	REQUIRE(run_cmd(vk, &f.pool, [&](VkCommandBuffer cmd) {
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, tl};
		vk_cmd_image_barrier_locked(vk, cmd, t.image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range);
		VkClearColorValue magenta = {};
		magenta.float32[0] = 1.0f;
		magenta.float32[2] = 1.0f;
		magenta.float32[3] = 1.0f;
		vk->vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &range);
		vk_conv_viewsyn_inputs in = {};
		in.color = f.color.view;
		in.color_width = kW;
		in.color_height = kH;
		in.depth = f.depth.view;
		in.depth_width = kDW;
		in.depth_height = kDH;
		in.input_layout = VK_IMAGE_LAYOUT_GENERAL;
		in.new_content = new_content;
		vk_conv_viewsyn_target tg = {};
		tg.image = t.image;
		tg.format = t.format;
		tg.width = tw;
		tg.height = kH;
		tg.array_layers = tl;
		tg.layout = VK_IMAGE_LAYOUT_GENERAL;
		tg.layout_kind = layout;
		rec = vk_conv_viewsyn_record(f.vs, cmd, &in, &p, &tg);
		return true;
	}));
	if (!rec) {
		return false;
	}
	std::vector<uint16_t> raw((size_t)tw * kH * tl * 4);
	REQUIRE(readback(vk, &f.pool, t, raw.data(), raw.size() * 2));
	views.assign(n, std::vector<Px>((size_t)kW * kH));
	for (uint32_t s = 0; s < tl; s++) {
		for (uint32_t y = 0; y < kH; y++) {
			for (uint32_t x = 0; x < tw; x++) {
				const uint32_t vi = layout == XRT_CONV_VIEWS_ROW ? x / kW : s;
				const uint32_t vx = layout == XRT_CONV_VIEWS_ROW ? x % kW : x;
				Px &px = views[vi][(size_t)y * kW + vx];
				memcpy(px.raw, &raw[(((size_t)s * kH + y) * tw + x) * 4], 8);
				px.r = half_bits_to_float(px.raw[0]);
				px.g = half_bits_to_float(px.raw[1]);
				px.b = half_bits_to_float(px.raw[2]);
			}
		}
	}
	return true;
}

u_conv_viewsyn_params
make_params(const std::vector<double> &eye_x_m)
{
	u_conv_viewsyn_params p;
	u_conv_viewsyn_params_defaults(&p);
	p.rect_width_m = kRectW;
	p.rect_height_m = kRectH;
	p.view_count = (uint32_t)eye_x_m.size();
	for (uint32_t i = 0; i < p.view_count; i++) {
		p.viewpoints_m[i][0] = (float)eye_x_m[i];
		p.viewpoints_m[i][1] = 0.0f;
		p.viewpoints_m[i][2] = 0.6f;
	}
	p.depth.kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
	p.range_near = 1.0f;
	p.range_far = 0.0f;
	p.convergence = 0.5f;
	p.budget = 0.1f;
	return p;
}

//! GPU vs CPU golden, statistically (see the file comment).
void
check_golden(Fixture &f, const u_conv_viewsyn_params &p, const std::vector<std::vector<Px>> &views)
{
	std::vector<Tex> ref;
	cpu_viewsyn(f.color_cpu, f.depth_cpu, kDW, kDH, p, kW, kH, ref);
	REQUIRE(ref.size() == views.size());
	for (size_t i = 0; i < views.size(); i++) {
		double sum = 0.0, maxe = 0.0;
		size_t over1 = 0, over4 = 0;
		for (uint32_t y = 0; y < kH; y++) {
			for (uint32_t x = 0; x < kW; x++) {
				const Px &g = views[i][(size_t)y * kW + x];
				const float *r = ref[i].at((int)x, (int)y);
				const double e =
				    std::max({std::fabs(g.r - r[0]), std::fabs(g.g - r[1]), std::fabs(g.b - r[2])});
				sum += e;
				maxe = std::max(maxe, e);
				over1 += e > 1.0 / 255.0 ? 1 : 0;
				over4 += e > 4.0 / 255.0 ? 1 : 0;
			}
		}
		const double n = (double)kW * kH;
		const double mean = sum / n;
		INFO("view " << i << ": mean |err| " << mean * 255.0 << "/255, max " << maxe * 255.0 << "/255, >1/255 "
		             << 100.0 * over1 / n << "%, >4/255 " << 100.0 * over4 / n << "%");
		CHECK(mean * 255.0 < 0.15);
		CHECK((double)over4 / n < 0.0025);
		// Report it even on success (ctest --success prints it).
		SUCCEED("view " << i << ": mean " << mean * 255.0 << "/255, max " << maxe * 255.0 << "/255, >1/255 "
		                << 100.0 * over1 / n << "%, >4/255 " << 100.0 * over4 / n << "%");
	}
}

//! The step scene for the CPU march (source uv, v up).
double
step_sample(void *, double u, double v)
{
	const double x0 = (double)kSqX0 / kDW, x1 = (double)kSqX1 / kDW;
	const double v1 = 1.0 - (double)kSqY0 / kDH, v0 = 1.0 - (double)kSqY1 / kDH;
	return (u >= x0 && u < x1 && v > v0 && v <= v1) ? 1.0 : 0.0;
}

double
edge_col(const std::vector<Px> &img, uint32_t y, int from, int to)
{
	int step = to > from ? 1 : -1;
	for (int x = from; x != to; x += step) {
		double b0 = img[(size_t)y * kW + x].b, b1 = img[(size_t)y * kW + x + step].b;
		if ((b0 < 0.5) != (b1 < 0.5)) {
			double t = (0.5 - b0) / (b1 - b0);
			return x + 0.5 + step * t;
		}
	}
	return NAN;
}

//! tests_conv_viewsyn_d3d11.cpp's oracles, unchanged.
void
check_views(const u_conv_viewsyn_params &p, const std::vector<std::vector<Px>> &views)
{
	u_conv_viewsyn_frame fr;
	REQUIRE(u_conv_viewsyn_resolve(&p, &fr));
	const double q_near = fr.views[0].q_scale * (1.0 - fr.conv_n);
	const double q_far = fr.views[0].q_scale * (0.0 - fr.conv_n);
	const uint32_t yc = kH / 2;
	const double src_left = kSqX0 * 4.0, src_right = kSqX1 * 4.0;
	double prev_shift = INFINITY;
	for (uint32_t i = 0; i < p.view_count; i++) {
		const std::vector<Px> &img = views[i];
		INFO("view " << i << " eye.x " << fr.views[i].eye.x << " cam.x " << fr.cam.x);
		uint32_t clear = 0;
		for (const Px &px : img) {
			clear += (px.r == 1.0 && px.g == 0.0 && px.b == 1.0) ? 1 : 0;
		}
		REQUIRE(clear == 0);
		const double dx_eye = fr.views[i].eye.x - fr.cam.x;
		auto shift_px = [&](double q) { return -dx_eye * q / fr.aspect * kW; };
		for (uint32_t x : {kW / 2, 90u, kW - 90}) {
			const double uv[2] = {(x + 0.5) / kW, 1.0 - (yc + 0.5) / kH};
			double s[2];
			double q = u_conv_viewsyn_march(&fr, &fr.views[i], step_sample, nullptr, uv, s);
			const Px &px = img[(size_t)yc * kW + x];
			INFO("x " << x << " gpu u " << px.r << " cpu u " << s[0] << " q " << q);
			CHECK(std::fabs(px.r - s[0]) * kW <= 1.0);
			CHECK(std::fabs(px.g - s[1]) * kH <= 0.5);
			const bool on_square = x == kW / 2;
			CHECK(std::fabs(q - (on_square ? q_near : q_far)) <= std::fabs(q_near - q_far) / fr.steps);
			CHECK(std::fabs((uv[0] - px.r) * kW - shift_px(on_square ? q_near : q_far)) <= 1.0);
		}
		const double sh = shift_px(q_near);
		if (std::fabs(dx_eye) > 1e-9) {
			const bool moves_left = sh < 0;
			double got = moves_left ? edge_col(img, yc, kW / 2, 0) : edge_col(img, yc, kW / 2, kW - 1);
			double want = (moves_left ? src_left : src_right) + sh;
			INFO("edge got " << got << " want " << want << " shift " << sh);
			CHECK(std::fabs(got - want) <= 1.5);
			CHECK(sh < prev_shift);
			prev_shift = sh;
		}
	}
}

} // namespace

TEST_CASE("viewsyn vk: 2 views, SBS row, golden + step edge parallax", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, views));
	check_views(p, views);
	check_golden(f, p, views);
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 1);
}

TEST_CASE("viewsyn vk: 4 views, array layers, golden + step edge parallax", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.063, -0.021, 0.021, 0.063});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ARRAY, views));
	check_views(p, views);
	check_golden(f, p, views);
}

TEST_CASE("viewsyn vk: a view at the source camera is the identity", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({0.01});
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, views));
	double max_err = 0.0;
	for (uint32_t y = 0; y < kH; y++) {
		for (uint32_t x = 0; x < kW; x++) {
			const Px &px = views[0][(size_t)y * kW + x];
			max_err = std::fmax(max_err, std::fabs(px.r - (x + 0.5) / kW));
			max_err = std::fmax(max_err, std::fabs(px.g - (1.0 - (y + 0.5) / kH)));
		}
	}
	INFO("max |err| " << max_err * 255.0 << " /255");
	CHECK(max_err * 255.0 < 1.0);
}

TEST_CASE("viewsyn vk: metric depth, dilation, dilate_hi and a binding rear cap match the golden", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f, true)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	p.depth.kind = XRT_CONV_DEPTH_METRIC;
	p.range_near = 1.0f; // metres at n = 1
	p.range_far = 2.0f;  // metres at n = 0
	p.convergence = 1.4f;
	p.dilate = 2;
	p.dilate_hi = 2;
	p.rear_cap_m = 0.002f; // binding: compresses the ground's negative parallax
	u_conv_viewsyn_frame fr;
	REQUIRE(u_conv_viewsyn_resolve(&p, &fr));
	REQUIRE(fr.rear_k < 1.0);
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, views));
	check_golden(f, p, views);
}

TEST_CASE("viewsyn vk: no upsample (depth-res disparity) matches the golden", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	p.upsample = false;
	std::vector<std::vector<Px>> views;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ARRAY, views));
	check_golden(f, p, views);
}

TEST_CASE("viewsyn vk: passes 1-6 are cached until an input or a parameter they read changes", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	std::vector<std::vector<Px>> a, b;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, a, true));
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 1);
	// Same content, same prepass parameters: pass 7 only, same pixels.
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, b, false));
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 1);
	uint32_t diffs = 0;
	for (size_t i = 0; i < a.size(); i++) {
		for (size_t k = 0; k < a[i].size(); k++) {
			diffs += std::memcmp(a[i][k].raw, b[i][k].raw, sizeof(a[i][k].raw)) != 0 ? 1u : 0u;
		}
	}
	CHECK(diffs == 0u);
	// Convergence is a pass-7 parameter: still cached.
	p.convergence = 0.4f;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, b, false));
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 1);
	// The normalisation range feeds pass 1: re-run.
	p.range_near = 1.2f;
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, b, false));
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 2);
	// New content always re-runs.
	REQUIRE(render(f, p, XRT_CONV_VIEWS_ROW, b, true));
	CHECK(vk_conv_viewsyn_prepass_count(f.vs) == 3);
}

TEST_CASE("viewsyn vk: invalid geometry and targets are refused", "[conv_viewsyn][vk]")
{
	Fixture f;
	if (!make_fixture(f)) {
		SKIP("no Vulkan device available");
	}
	u_conv_viewsyn_params p = make_params({-0.0315, 0.0315});
	std::vector<std::vector<Px>> views;
	p.rect_height_m = 0.0f;
	CHECK_FALSE(render(f, p, XRT_CONV_VIEWS_ROW, views));
	p = make_params({-0.0315, 0.0315});
	CHECK_FALSE(render(f, p, 99u, views)); // unknown layout
}
