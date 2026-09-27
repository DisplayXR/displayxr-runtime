// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's FAKE lift module on the Vulkan / Android lift slots
 *         (see sim_display_lift_vk.h).
 * @ingroup drv_sim_display
 */

#include "sim_display_lift_vk.h"

#ifdef XRT_OS_ANDROID

#include "util/u_debug.h"
#include "util/u_logging.h"

#include "xrt/xrt_display_processor_vk.h"
#include "xrt/xrt_dp_lift.h"

#include "os/os_time.h"
#include "sim_display_fake_ply.h"

#include <android/hardware_buffer.h>
#include <sys/system_properties.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(sim_display_fake_lift_vk, "SIM_DISPLAY_FAKE_LIFT", false)
DEBUG_GET_ONCE_NUM_OPTION(sim_display_fake_lift_vk_latency_ms, "SIM_DISPLAY_FAKE_LIFT_LATENCY_MS", 8)

#define SIM_FAKE_LIFT_VK_MAX_STREAMS 8
#define SIM_FAKE_LIFT_VK_MAX_VIEWS 8

static bool
prop_is_one(const char *name)
{
	char v[PROP_VALUE_MAX] = {0};
	return __system_property_get(name, v) > 0 && (v[0] == '1' || v[0] == 't' || v[0] == 'y');
}

bool
sim_fake_lift_vk_enabled(void)
{
	static int s_enabled = -1;
	if (s_enabled < 0) {
		s_enabled =
		    (debug_get_bool_option_sim_display_fake_lift_vk() || prop_is_one("debug.dxr.lift.fake")) ? 1 : 0;
	}
	return s_enabled == 1;
}

static int64_t
fake_latency_ms(void)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get("debug.dxr.lift.fake_latency_ms", v) > 0 && v[0] != '\0') {
		long ms = strtol(v, NULL, 10);
		return ms < 0 ? 0 : (ms > 5000 ? 5000 : ms);
	}
	return debug_get_num_option_sim_display_fake_lift_vk_latency_ms();
}

struct fake_vk_stream
{
	bool used;
	uint64_t id;
	uint32_t mode;
	AHardwareBuffer *out; //!< DP-owned output, valid until the stream's next convert
	uint32_t out_w, out_h;
	uint8_t *blob;
	size_t blob_size;
};

struct sim_lift_vk_dp
{
	struct xrt_display_processor_vk base;
	uint64_t next_id;
	struct fake_vk_stream streams[SIM_FAKE_LIFT_VK_MAX_STREAMS];
};

static struct sim_lift_vk_dp *
sim_lift_vk_dp(struct xrt_display_processor_vk *xdp)
{
	return (struct sim_lift_vk_dp *)xdp;
}

static struct fake_vk_stream *
find_stream(struct sim_lift_vk_dp *d, uint64_t id)
{
	for (uint32_t i = 0; i < SIM_FAKE_LIFT_VK_MAX_STREAMS; i++) {
		if (d->streams[i].used && d->streams[i].id == id) {
			return &d->streams[i];
		}
	}
	return NULL;
}

static void
stream_release(struct fake_vk_stream *s)
{
	if (s->out != NULL) {
		AHardwareBuffer_release(s->out);
	}
	free(s->blob);
	memset(s, 0, sizeof(*s));
}

static void
fake_sleep(void)
{
	const int64_t ms = fake_latency_ms();
	if (ms > 0) {
		os_nanosleep(ms * 1000 * 1000);
	}
}

static bool
fake_get_caps(struct xrt_display_processor_vk *xdp, struct xrt_dp_lift_caps *out)
{
	(void)xdp;
	if (out == NULL || out->struct_size < sizeof(struct xrt_dp_lift_caps)) {
		return false;
	}
	out->modes =
	    XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW | XRT_DP_LIFT_MODE_GAUSSIANS;
	out->max_streams = SIM_FAKE_LIFT_VK_MAX_STREAMS;
	out->max_views = SIM_FAKE_LIFT_VK_MAX_VIEWS;
	out->depth_semantics = XRT_DP_LIFT_DEPTH_RELATIVE;
	out->state = XRT_DP_LIFT_STATE_READY;
	out->typical_latency_ns = (uint64_t)fake_latency_ms() * 1000000ull;
	snprintf(out->backend, sizeof(out->backend), "sim_display-fake-vk");
	return true;
}

static bool
fake_stream_create(struct xrt_display_processor_vk *xdp, const struct xrt_dp_lift_stream_info *info, uint64_t *out_id)
{
	struct sim_lift_vk_dp *d = sim_lift_vk_dp(xdp);
	if (info == NULL || out_id == NULL) {
		return false;
	}
	const uint32_t m = info->mode;
	if (m != XRT_DP_LIFT_MODE_DEPTH && m != XRT_DP_LIFT_MODE_SBS && m != XRT_DP_LIFT_MODE_NVIEW &&
	    m != XRT_DP_LIFT_MODE_GAUSSIANS) {
		return false;
	}
	for (uint32_t i = 0; i < SIM_FAKE_LIFT_VK_MAX_STREAMS; i++) {
		struct fake_vk_stream *s = &d->streams[i];
		if (!s->used) {
			memset(s, 0, sizeof(*s));
			s->used = true;
			s->id = ++d->next_id;
			s->mode = m;
			*out_id = s->id;
			return true;
		}
	}
	return false;
}

static void
fake_stream_destroy(struct xrt_display_processor_vk *xdp, uint64_t id)
{
	struct fake_vk_stream *s = find_stream(sim_lift_vk_dp(xdp), id);
	if (s != NULL) {
		stream_release(s);
	}
}

static bool
ensure_out(struct fake_vk_stream *s, uint32_t w, uint32_t h, bool cpu_read)
{
	if (s->out != NULL && s->out_w == w && s->out_h == h) {
		return true;
	}
	if (s->out != NULL) {
		AHardwareBuffer_release(s->out);
		s->out = NULL;
	}
	AHardwareBuffer_Desc desc = {0};
	desc.width = w;
	desc.height = h;
	desc.layers = 1;
	desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
	if (cpu_read) {
		// DEPTH: CPU-readable like the runtime's DEPTH export (XR_DXR_lift § Android).
		desc.usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
	}
	if (AHardwareBuffer_allocate(&desc, &s->out) != 0) {
		U_LOG_E("sim_display fake lift (vk): output AHardwareBuffer %ux%u allocate failed", w, h);
		s->out = NULL;
		return false;
	}
	s->out_w = w;
	s->out_h = h;
	return true;
}

//! Lock @p ahb for CPU reading; returns the pixels and the row stride in PIXELS.
static const uint8_t *
lock_read(AHardwareBuffer *ahb, uint32_t *out_stride_px)
{
	AHardwareBuffer_Desc desc = {0};
	AHardwareBuffer_describe(ahb, &desc);
	void *ptr = NULL;
	if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &ptr) != 0 || ptr == NULL) {
		return NULL;
	}
	*out_stride_px = desc.stride;
	return (const uint8_t *)ptr;
}

static bool
fake_convert(struct xrt_display_processor_vk *xdp,
             uint64_t id,
             void *input_buffer,
             VkImage_XDP input_image,
             uint32_t w,
             uint32_t h,
             const struct xrt_dp_lift_params *p,
             const float *viewpoints_xyz,
             uint32_t viewpoint_floats,
             void **out_buffer,
             VkImage_XDP *out_image,
             uint32_t *out_w,
             uint32_t *out_h,
             uint32_t *out_format)
{
	(void)input_image;
	(void)viewpoints_xyz;
	(void)viewpoint_floats;
	struct fake_vk_stream *s = find_stream(sim_lift_vk_dp(xdp), id);
	if (s == NULL || s->mode == XRT_DP_LIFT_MODE_GAUSSIANS || input_buffer == NULL || w == 0 || h == 0 ||
	    out_buffer == NULL || out_image == NULL || out_w == NULL || out_h == NULL || out_format == NULL) {
		return false;
	}
	uint32_t views = 1;
	if (s->mode == XRT_DP_LIFT_MODE_SBS) {
		views = 2;
	} else if (s->mode == XRT_DP_LIFT_MODE_NVIEW) {
		views = (p != NULL && p->view_count >= 1) ? p->view_count : 4;
		views = views > SIM_FAKE_LIFT_VK_MAX_VIEWS ? SIM_FAKE_LIFT_VK_MAX_VIEWS : views;
	}
	const bool depth = s->mode == XRT_DP_LIFT_MODE_DEPTH;
	const uint32_t ow = w * views;
	const uint64_t t0 = os_monotonic_get_ns();
	if (!ensure_out(s, ow, h, depth)) {
		return false;
	}

	void *dst_ptr = NULL;
	if (AHardwareBuffer_lock(s->out, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &dst_ptr) != 0 ||
	    dst_ptr == NULL) {
		return false;
	}
	AHardwareBuffer_Desc od = {0};
	AHardwareBuffer_describe(s->out, &od);
	uint8_t *dst = (uint8_t *)dst_ptr;
	const size_t dst_pitch = (size_t)od.stride * 4u;

	bool ok = true;
	if (depth) {
		// RELATIVE depth, larger = farther: 0 at the top, 255 at the bottom.
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t g = (uint8_t)((y * 255u) / (h > 1 ? h - 1 : 1));
			uint8_t *row = dst + (size_t)y * dst_pitch;
			for (uint32_t x = 0; x < ow; x++) {
				row[4 * x + 0] = g;
				row[4 * x + 1] = g;
				row[4 * x + 2] = g;
				row[4 * x + 3] = 255;
			}
		}
	} else {
		uint32_t src_stride_px = 0;
		const uint8_t *src = lock_read((AHardwareBuffer *)input_buffer, &src_stride_px);
		if (src == NULL) {
			ok = false;
		} else {
			// ~2% of the view width per view step at strength 1 — the D3D11
			// fake's constant parallax: visible, obviously fake.
			const float strength = (p != NULL && p->strength > 0.0f) ? p->strength : 1.0f;
			const float shift = 0.02f * strength;
			const size_t src_pitch = (size_t)src_stride_px * 4u;
			for (uint32_t v = 0; v < views; v++) {
				const float off_f = ((float)v - (float)(views - 1) * 0.5f) * shift * (float)w;
				const int32_t off = (int32_t)(off_f >= 0.0f ? off_f + 0.5f : off_f - 0.5f);
				for (uint32_t y = 0; y < h; y++) {
					const uint8_t *srow = src + (size_t)y * src_pitch;
					uint8_t *drow = dst + (size_t)y * dst_pitch + (size_t)v * w * 4u;
					// Sample x + off, clamped to the edge (the D3D11 fake's saturate()).
					int32_t x0 = -off; // first x whose source is >= 0
					x0 = x0 < 0 ? 0 : (x0 > (int32_t)w ? (int32_t)w : x0);
					int32_t x1 = (int32_t)w - off; // first x whose source is >= w
					x1 = x1 < x0 ? x0 : (x1 > (int32_t)w ? (int32_t)w : x1);
					for (int32_t x = 0; x < x0; x++) {
						memcpy(drow + 4 * x, srow, 4);
					}
					if (x1 > x0) {
						memcpy(drow + 4 * x0, srow + 4 * (x0 + off), (size_t)(x1 - x0) * 4u);
					}
					for (int32_t x = x1; x < (int32_t)w; x++) {
						memcpy(drow + 4 * x, srow + 4 * (w - 1), 4);
					}
				}
			}
			AHardwareBuffer_unlock((AHardwareBuffer *)input_buffer, NULL);
		}
	}
	AHardwareBuffer_unlock(s->out, NULL); // synchronous: the CPU writes are done
	if (!ok) {
		return false;
	}
	// The fake's own CPU cost (locks + copies), separate from the latency sleep:
	// what bounds its rate when fake_latency_ms is 0. WARN (a plug-in's INFO never
	// reaches logcat), throttled: the first 5 conversions, then every 300th.
	static uint32_t s_timed = 0;
	if (s_timed++ < 5 || s_timed % 300 == 0) {
		U_LOG_W("sim_display fake lift (vk): %ux%u x%u views, CPU %.2f ms + sleep %lld ms", w, h, views,
		        (double)(os_monotonic_get_ns() - t0) / 1e6, (long long)fake_latency_ms());
	}

	fake_sleep();
	*out_buffer = s->out;
	*out_image = (VkImage_XDP)0; // the runtime imports the buffer itself
	*out_w = ow;
	*out_h = h;
	*out_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	return true;
}

static bool
fake_convert_blob(struct xrt_display_processor_vk *xdp,
                  uint64_t id,
                  void *input_buffer,
                  VkImage_XDP input_image,
                  uint32_t w,
                  uint32_t h,
                  const struct xrt_dp_lift_params *p,
                  uint32_t *out_format,
                  const void **out_bytes,
                  size_t *out_size)
{
	(void)input_image;
	struct fake_vk_stream *s = find_stream(sim_lift_vk_dp(xdp), id);
	if (s == NULL || s->mode != XRT_DP_LIFT_MODE_GAUSSIANS || input_buffer == NULL || out_format == NULL ||
	    out_bytes == NULL || out_size == NULL) {
		return false;
	}
	const float focal = p != NULL ? p->focal_px : 0.0f; // <= 0: the generator's default FOV
	const size_t need = sim_fake_ply_write(NULL, 0, NULL, w, h, 0, focal);
	if (s->blob_size != need) {
		free(s->blob);
		s->blob = (uint8_t *)malloc(need);
		s->blob_size = s->blob != NULL ? need : 0;
	}
	if (s->blob == NULL) {
		return false;
	}
	uint32_t stride_px = 0;
	const uint8_t *rgba = lock_read((AHardwareBuffer *)input_buffer, &stride_px);
	sim_fake_ply_write(s->blob, s->blob_size, rgba, w, h, stride_px * 4u, focal);
	if (rgba != NULL) {
		AHardwareBuffer_unlock((AHardwareBuffer *)input_buffer, NULL);
	}
	fake_sleep(); // photo -> splats is seconds on a real module; the fake is quick but not free
	*out_format = XRT_DP_LIFT_BLOB_PLY_3DGS;
	*out_bytes = s->blob;
	*out_size = s->blob_size;
	return true;
}

static void
fake_destroy(struct xrt_display_processor *xdp)
{
	struct sim_lift_vk_dp *d = (struct sim_lift_vk_dp *)xdp;
	for (uint32_t i = 0; i < SIM_FAKE_LIFT_VK_MAX_STREAMS; i++) {
		if (d->streams[i].used) {
			stream_release(&d->streams[i]);
		}
	}
	free(d);
}

xrt_result_t
sim_display_dp_factory_vk_lift(void *vk_bundle,
                               void *vk_cmd_pool,
                               void *window_handle,
                               int32_t target_format,
                               struct xrt_display_processor **out_xdp)
{
	(void)vk_bundle;
	(void)vk_cmd_pool;
	(void)window_handle;
	(void)target_format;
	if (out_xdp == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_xdp = NULL;
	if (!sim_fake_lift_vk_enabled()) {
		U_LOG_I("sim_display: lift-only VK DP refused (fake lift off: set debug.dxr.lift.fake=1)");
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct sim_lift_vk_dp *d = (struct sim_lift_vk_dp *)calloc(1, sizeof(*d));
	if (d == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	// The variant, covering the lift slots. Every base slot but destroy stays
	// NULL: a lift-only DP is never asked to weave (xrt_plugin_iface::create_dp_vk_lift).
	d->base.base.struct_size = (uint32_t)sizeof(struct xrt_display_processor_vk);
	d->base.base.destroy = fake_destroy;
	d->base.lift_get_caps = fake_get_caps;
	d->base.lift_stream_create = fake_stream_create;
	d->base.lift_stream_destroy = fake_stream_destroy;
	d->base.lift_convert = fake_convert;
	d->base.lift_convert_blob = fake_convert_blob;
	U_LOG_W("sim_display: FAKE lift module (vk/AHardwareBuffer, CPU) — latency %lld ms",
	        (long long)fake_latency_ms());
	*out_xdp = &d->base.base;
	return XRT_SUCCESS;
}

#endif // XRT_OS_ANDROID
