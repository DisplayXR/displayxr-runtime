// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MJPEG -> NV12 for the V4L2 UVC capture backend (os_uvc_capture_v4l2.c,
 *         ADR-043 Amendment 4).
 *
 * Media Foundation decodes MJPEG for the Windows backend; V4L2 hands over the
 * compressed frame, so desktop Linux decodes it here, on the CPU, into the
 * NV12 the source splits. Two decoders, best first, chosen per open:
 *
 *  - **libturbojpeg** (`libturbojpeg.so.0`, dlopen'd — the runtime keeps no
 *    hard dependency, so the .deb's DT_NEEDED list is unchanged): decodes
 *    straight to the JPEG's own Y/Cb/Cr planes (no RGB round trip) and uses
 *    DCT-domain scaling to land as close ABOVE the requested output size as
 *    one of its scaling factors allows (half-SBS 3840x2160 -> 2880x1620 for a
 *    2560x720 output), which is most of the decode cost saved.
 *  - **built-in** (stb_image, compiled in): full-size decode to RGB, converted
 *    back to full-range BT.601 YCbCr (the JFIF transform, so it recovers the
 *    JPEG's own YCbCr up to rounding). Slow — a 4K frame is ~100 ms — and the
 *    open logs one WARN naming libturbojpeg as the fix.
 *
 * Both deliver the JPEG's YCbCr unchanged in range (full range), as Media
 * Foundation's MJPEG decoder does on Windows. UVC MJPEG frames commonly omit
 * the Huffman tables (the AVI1 "default tables" convention); a frame without a
 * DHT segment gets the standard tables (JPEG Annex K.3) inserted before
 * decoding (@ref os_uvc_mjpeg_insert_dht) — libjpeg-turbo copes on its own,
 * stb_image does not.
 *
 * The pure helpers are exported for tests_uvc_capture_v4l2.
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

enum os_uvc_mjpeg_impl
{
	OS_UVC_MJPEG_AUTO = 0,      //!< libturbojpeg when it loads, else built-in
	OS_UVC_MJPEG_TURBOJPEG = 1, //!< libturbojpeg only (create fails without it)
	OS_UVC_MJPEG_BUILTIN = 2,   //!< stb_image only
};

struct os_uvc_mjpeg;

/*!
 * Is libturbojpeg loadable? @p why (may be NULL) gets the library path or the
 * dlopen error. Loads it once per process; touches no camera.
 */
bool
os_uvc_mjpeg_turbojpeg_available(char *why, size_t why_cap);

//! A decoder (one per open device; not thread-safe). NULL when @p impl cannot be had.
struct os_uvc_mjpeg *
os_uvc_mjpeg_create(enum os_uvc_mjpeg_impl impl);

//! "libturbojpeg" | "built-in (stb_image)".
const char *
os_uvc_mjpeg_name(const struct os_uvc_mjpeg *d);

/*!
 * Decode one JPEG into NV12. @p want_w x @p want_h is the frame size the
 * source resamples to (0 = no preference): libturbojpeg scales down to the
 * smallest of its factors that stays at or above it. The output width is a
 * multiple of 4 and the height even (an odd remainder is cropped), as
 * u_stereo_uvc_split() requires. @p out's planes stay valid until the next
 * decode / destroy. @return false on a corrupt frame (the caller drops it).
 */
bool
os_uvc_mjpeg_decode(struct os_uvc_mjpeg *d,
                    const uint8_t *jpg,
                    size_t len,
                    uint32_t want_w,
                    uint32_t want_h,
                    struct u_stereo_uvc_raw_frame *out);

void
os_uvc_mjpeg_destroy(struct os_uvc_mjpeg **d_ptr);


/*
 *
 * Pure helpers (tested).
 *
 */

//! The frame size from the first SOF0..SOF15 (not DHT/JPG/DAC) marker before SOS.
bool
os_uvc_mjpeg_header(const uint8_t *jpg, size_t len, uint32_t *out_w, uint32_t *out_h);

//! Does the JPEG carry a DHT (Huffman table) segment before its first SOS?
bool
os_uvc_mjpeg_has_dht(const uint8_t *jpg, size_t len);

//! Bytes the standard DHT segment adds (@ref os_uvc_mjpeg_insert_dht).
#define OS_UVC_MJPEG_DHT_SIZE 420u

/*!
 * Copy @p jpg into @p out with the standard Huffman tables (JPEG Annex K.3,
 * one DHT segment, @ref OS_UVC_MJPEG_DHT_SIZE bytes) inserted right after SOI.
 * @return the new length, 0 if @p jpg does not start with SOI or @p cap is
 * short (needs len + OS_UVC_MJPEG_DHT_SIZE).
 */
size_t
os_uvc_mjpeg_insert_dht(const uint8_t *jpg, size_t len, uint8_t *out, size_t cap);

/*!
 * DCT-domain scale choice: of @p count factors (@p num[i] / @p den[i]), the
 * SMALLEST <= 1 whose scaled size ceil(w * num / den) x ceil(h * num / den)
 * is >= @p want_w x @p want_h, with a scaled width that is a multiple of 4
 * and an even height. 1/1 when @p want_w or @p want_h is 0 or nothing smaller
 * qualifies.
 */
void
os_uvc_mjpeg_pick_scale(const int *num,
                        const int *den,
                        uint32_t count,
                        uint32_t w,
                        uint32_t h,
                        uint32_t want_w,
                        uint32_t want_h,
                        uint32_t *out_num,
                        uint32_t *out_den);

/*!
 * Planar Cb/Cr at any JPEG subsampling — one chroma sample per @p hf x @p vf
 * luma pixels (1, 2 or 4 each; @p pitch bytes per chroma row) — into the
 * interleaved UV plane of a @p w x @p h NV12 image (w a multiple of 4, h even):
 * each NV12 sample is the mean of the source samples under its 2x2 luma block,
 * so 4:2:0 copies, 4:2:2 averages row pairs and 4:4:4 averages 2x2. @p u /
 * @p v NULL (greyscale) = neutral 128.
 */
void
os_uvc_mjpeg_chroma_to_nv12(const uint8_t *u,
                            const uint8_t *v,
                            uint32_t pitch,
                            uint32_t hf,
                            uint32_t vf,
                            uint32_t w,
                            uint32_t h,
                            uint8_t *uv,
                            uint32_t uv_pitch);

/*!
 * Packed RGB (3 bytes/px) -> full-range BT.601 NV12 (the JFIF transform; the
 * chroma of each 2x2 block is the mean of its four pixels). w a multiple of 2,
 * h even.
 */
void
os_uvc_mjpeg_rgb_to_nv12(const uint8_t *rgb,
                         uint32_t rgb_pitch,
                         uint32_t w,
                         uint32_t h,
                         uint8_t *y,
                         uint32_t y_pitch,
                         uint8_t *uv,
                         uint32_t uv_pitch);

#ifdef __cplusplus
}
#endif
