// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043) platform-neutral building blocks:
 *         the per-stream latest-wins pinned ring, the frame-rate decimator,
 *         plane layout + format conversion, the per-consumer persistent id,
 *         and a block-matching disparity probe.
 *
 * Pure logic — no threads, no OS handles — so the service's camera manager
 * (ipc_server_stereo_camera.c), the sim_display fake and `displayxr-cli
 * camera` share one tested implementation (tests_stereo_camera).
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Latest-wins ring with one consumer pin.
 *
 * One ring per stream. The camera thread writes into a slot that is neither
 * the latest published slot nor the one the consumer has pinned, then
 * publishes it. An acquire hands out the latest slot IF it is newer than the
 * last one this stream acquired and pins it: the slot stays byte-stable until
 * the next acquire. Three slots are therefore always enough. Frames the
 * consumer did not acquire in time are skipped, never queued.
 *
 */

#define U_STEREO_CAMERA_RING_SLOTS 3

struct u_stereo_camera_ring
{
	int32_t latest;             //!< slot of the newest published frame, -1 = none
	int32_t pinned;             //!< slot the consumer holds, -1 = none
	int32_t writing;            //!< slot handed out by begin_write, -1 = none
	uint64_t latest_seq;        //!< sequence of @ref latest (0 = none)
	uint64_t last_acquired_seq; //!< sequence of the last acquire (0 = none)
	bool latest_acquired;       //!< the consumer has taken @ref latest

	uint64_t published; //!< frames published to this ring
	uint64_t skipped;   //!< published frames superseded before any acquire
	uint64_t acquired;  //!< successful acquires
};

void
u_stereo_camera_ring_init(struct u_stereo_camera_ring *r);

//! A slot that is neither latest nor pinned. Always succeeds (3 slots).
int32_t
u_stereo_camera_ring_begin_write(struct u_stereo_camera_ring *r);

//! Publish the slot from begin_write as the newest frame @p seq (> 0).
void
u_stereo_camera_ring_publish(struct u_stereo_camera_ring *r, int32_t slot, uint64_t seq);

//! Abandon a begin_write (conversion failed): the slot stays free.
void
u_stereo_camera_ring_abort_write(struct u_stereo_camera_ring *r);

/*!
 * Pin and return the newest frame if newer than the last acquire. The
 * previous pin is released first (it is only valid until the next acquire).
 * @return false = NOT READY (the previous pin is kept).
 */
bool
u_stereo_camera_ring_acquire(struct u_stereo_camera_ring *r, int32_t *out_slot, uint64_t *out_seq);

/*!
 * Drop the pin and forget the latest frame — used on suspension so the last
 * image does not linger for a consumer that may no longer see frames.
 */
void
u_stereo_camera_ring_clear(struct u_stereo_camera_ring *r);


/*
 *
 * Decimation: never interpolate, never burst.
 *
 */

struct u_stereo_camera_decimator
{
	int64_t period_ns; //!< 0 = pass everything
	int64_t next_due_ns;
	bool started;
};

//! @p max_rate <= 0 or >= @p source_rate = pass everything.
void
u_stereo_camera_decimator_init(struct u_stereo_camera_decimator *d, float max_rate, float source_rate);

//! Should the frame stamped @p t_ns be delivered?
bool
u_stereo_camera_decimator_accept(struct u_stereo_camera_decimator *d, int64_t t_ns);


/*
 *
 * Plane layout + conversion (GRAY8 = 1, NV12 = 2, BGRA8 = 3; the
 * xrt_stereo_camera_format values).
 *
 */

struct u_stereo_camera_planes
{
	uint32_t plane_count;
	uint32_t pitch[2];
	uint64_t offset[2];
	uint64_t size; //!< bytes the whole image needs
};

/*!
 * Tight layout of a @p width x @p height image, pitches rounded up to 64 bytes
 * (GPU-upload and SIMD friendly). NV12 needs even width and height.
 * @return false on an unknown format or odd NV12 extent.
 */
bool
u_stereo_camera_layout(uint32_t format, uint32_t width, uint32_t height, struct u_stereo_camera_planes *out);

/*!
 * Convert one SBS image between formats. BT.601 limited range for the
 * luma<->RGB legs; a GRAY8 source becomes NV12 with neutral (128) chroma.
 * @return false on an unsupported pair or bad arguments.
 */
bool
u_stereo_camera_convert(uint32_t src_format,
                        const uint8_t *const src_planes[2],
                        const uint32_t src_pitches[2],
                        uint32_t dst_format,
                        uint8_t *dst,
                        const struct u_stereo_camera_planes *dst_layout,
                        uint32_t width,
                        uint32_t height);


/*
 *
 * Identity + probe helpers.
 *
 */

/*!
 * A per-(device, consumer) id: "dxrcam-" + 32 hex chars. Never the raw device
 * identity (fingerprinting, spec §7.5).
 *
 * R1: an unkeyed 128-bit FNV-1a pair over (device_identity, consumer). R3
 * keys it with a persisted per-user secret so ids cannot be recomputed from a
 * known serial.
 */
void
u_stereo_camera_persistent_id(const char *device_identity, const char *consumer, char out[64]);

/*!
 * Horizontal disparity (left x - right x, pixels, sub-pixel) of the region
 * [x0, x0+w) x [y0, y0+h) of the LEFT eye of a GRAY8 SBS image, by SAD block
 * matching over 0..@p max_disparity. Used by `displayxr-cli camera probe` and
 * the tests to check a fake source's known disparity.
 * @return false if the region does not fit.
 */
bool
u_stereo_camera_estimate_disparity(const uint8_t *gray,
                                   uint32_t pitch,
                                   uint32_t eye_width,
                                   uint32_t height,
                                   uint32_t x0,
                                   uint32_t y0,
                                   uint32_t w,
                                   uint32_t h,
                                   uint32_t max_disparity,
                                   float *out_disparity);

#ifdef __cplusplus
}
#endif
