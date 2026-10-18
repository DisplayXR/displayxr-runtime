// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Internals of the V4L2 UVC capture backend (os_uvc_capture_v4l2.c)
 *         exported for tests_uvc_capture_v4l2 — the parts that need no device.
 *
 * The backend itself is reached through os_uvc_capture_backend() like every
 * other platform's (os_uvc_capture.h).
 *
 * @ingroup aux_os
 */

#pragma once

#include "util/u_stereo_uvc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One (pixel format, size, rate) a V4L2 device offers.
struct os_uvc_v4l2_format
{
	uint32_t fourcc; //!< V4L2_PIX_FMT_*
	uint32_t width, height;
	float fps; //!< 0 = unknown
};

/*!
 * List the video capture devices under @p sysfs_dir (normally
 * /sys/class/video4linux) WITHOUT opening any of them — the analogue of
 * MFEnumDeviceSources. Per `videoN`:
 *  - capture or not: udev's `ID_V4L_CAPABILITIES` from `<udev_dir>/c<maj>:<min>`
 *    (udev's v4l_id probed the node at plug time); without a udev record, the
 *    sysfs `index` attribute (0 = the first node of a device; UVC's metadata
 *    node is index 1);
 *  - USB VID:PID + product string: from the USB device the node's `device`
 *    link resolves under (the interface's parent);
 *  - name: @ref os_uvc_v4l2_friendly_name; id: `<dev_dir>/videoN`.
 * USB-backed devices are listed first (in node order), then the rest, so a
 * laptop's many ISP nodes never crowd a USB webcam out of the
 * U_STEREO_UVC_MAX_DEVICES cap.
 */
uint32_t
os_uvc_v4l2_enumerate_at(
    const char *sysfs_dir, const char *udev_dir, const char *dev_dir, struct u_stereo_uvc_device *out, uint32_t cap);

/*!
 * The friendly name a config's "match" substring is tested against: the V4L2
 * card name (uvcvideo: "<USB product>: <entity>", truncated by the kernel to
 * 31 bytes) when it contains the USB product string, else
 * "<product> (<card>)" — so a substring written against the Windows friendly
 * name (the USB product) matches on Linux too. Either may be NULL / "".
 */
void
os_uvc_v4l2_friendly_name(const char *card, const char *product, char *out, size_t cap);

/*!
 * From a udev database record (`E:KEY=VALUE` lines): 1 when
 * ID_V4L_CAPABILITIES lists `:capture:`, 0 when it is present without it, -1
 * when the record has no such key.
 */
int
os_uvc_v4l2_udev_capture(const char *record);

//! NV12 3 > YUYV 2 > MJPEG / JPEG 1 (needs a decoder) > anything else 0 (unusable) — Windows' order.
int
os_uvc_v4l2_fourcc_rank(uint32_t fourcc);

/*!
 * The format to open for @p want (Windows' pick_native): the exact size, then
 * the rate closest to @p want->fps (0 = the fastest), ties to the better rank.
 * Formats of rank 0 never qualify. @p allow_mjpeg false skips MJPEG / JPEG
 * (the "hardware only" decoder policy has no MJPEG decoder on Linux).
 */
bool
os_uvc_v4l2_pick_format(const struct os_uvc_v4l2_format *formats,
                        uint32_t count,
                        const struct u_stereo_uvc_mode *want,
                        bool allow_mjpeg,
                        struct os_uvc_v4l2_format *out);

//! The modes list_modes reports: every usable format's (size, rate), duplicates across pixel formats folded.
uint32_t
os_uvc_v4l2_modes_from_formats(const struct os_uvc_v4l2_format *formats,
                               uint32_t count,
                               struct u_stereo_uvc_mode *out,
                               uint32_t cap);

/*!
 * Split a concatenated MJPEG stream (a `.mjpeg` file, e.g. `ffmpeg -f mjpeg`)
 * into its frames: SOI .. EOI, the EOI found by walking the marker segments to
 * SOS and then the entropy-coded data (so an embedded thumbnail's EOI inside
 * an APP segment does not end the frame). Bytes between frames are skipped.
 * @return the frame count (at most @p cap recorded).
 */
uint32_t
os_uvc_v4l2_mjpeg_split_stream(const uint8_t *data, size_t len, size_t *offsets, size_t *lengths, uint32_t cap);

#ifdef __cplusplus
}
#endif
