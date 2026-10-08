// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The OS capture backend of the vendor-neutral UVC stereo camera source
 *         (u_stereo_uvc.h, ADR-043 Amendment 4).
 *
 * One function per platform fills a @ref u_stereo_uvc_backend:
 *  - Windows: Media Foundation (os_uvc_capture_mf.cpp) — devices from
 *    MFEnumDeviceSources (no device is activated to list them), modes from the
 *    device's media types (the device object is created, never started), and
 *    an ASYNC IMFSourceReader per open device whose callback keeps only the
 *    newest sample; MJPEG / YUY2 / NV12 are decoded / converted to NV12 by the
 *    reader's own decoder MFT (advanced video processing), never by us.
 *  - Linux desktop: V4L2 — TODO (os_uvc_capture_stubs.c reports no backend).
 *  - Everything else: no backend; the source then only ever sees the fake.
 *
 * A separate static library (aux_os_uvc) so the capture DLL imports land only
 * in the processes that capture: the service and displayxr-cli.
 *
 * @ingroup aux_os
 */

#pragma once

#include "util/u_stereo_uvc.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Fill @p out with this platform's capture backend.
 * @return false when the platform has none (@p out is zeroed).
 */
bool
os_uvc_capture_backend(struct u_stereo_uvc_backend *out);

/*!
 * Human-readable report of the decode path open() would take (`displayxr-cli
 * camera uvc-devices --decoder`): the decoder policy, the hardware / software
 * MJPEG decoders registered, the adapters and which one would decode. Touches
 * no camera. A platform without a backend says so.
 */
void
os_uvc_capture_decoder_report(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
