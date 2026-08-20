// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  External background-capture receiver for compose-under (#1073 T2).
 * @author David Fattal
 * @ingroup comp_multi
 *
 * Tier 2 of `docs/roadmap/android-transparency-compose-under.md`: the *real*
 * background producer. T0 draws a static backdrop in the runtime; T2 receives
 * **actual screen-behind-the-window pixels** from a privileged producer and
 * pushes them through the very same seam
 * (`xrt_display_processor::set_background_2d`, base DP vtable slot 16).
 *
 * ## Why the runtime is the *listener*, and why the producer is out of process
 *
 * No Android API lets an app capture the screen behind its own layer:
 * `READ_FRAME_BUFFER` is `signature|recents`, `CAPTURE_VIDEO_OUTPUT` and
 * `ACCESS_SURFACE_FLINGER` are plain `signature` — **none is
 * `signature|privileged`**, so `/system/priv-app` plus a `privapp-permissions`
 * allowlist cannot grant them either. Capture therefore always happens in a
 * process we do not own: a platform-signed vendor service (the product tier,
 * see the L10/L12 ask on displayxr-runtime#1038) or, for development, the
 * capture daemon shipped in the vendor display-config APK and launched at
 * shell/root uid.
 *
 * The runtime **listens** rather than connects so the producer is free to
 * start, die and restart underneath a live session without the compositor ever
 * noticing: no frame simply means no background, which is byte-for-byte the
 * pre-#1073 path. That is the whole probe/fallback story — there is no
 * handshake to fail and nothing to time out.
 *
 * ## Wire protocol (v1)
 *
 * `AF_UNIX`/`SOCK_STREAM` on the **abstract** namespace (Linux + Android), so
 * there is no filesystem path to chown and nothing to clean up. Default name
 * `@displayxr.bg2d`.
 *
 * ```text
 *   producer → runtime, once:      { u32 magic = 'DXRB', u32 version = 1 }
 *   producer → runtime, per frame: { u32 magic = 'DXRF', u32 seq,
 *                                    u32 width, u32 height,
 *                                    u32 stride_bytes, u32 format,
 *                                    u32 payload_bytes }  followed by
 *                                  payload_bytes of pixels
 * ```
 *
 * `format` is 0 = `R8G8B8A8_UNORM`, opaque (so premultiplied and straight
 * agree, satisfying slot 16's premultiplied contract by construction).
 * `stride_bytes` may exceed `width * 4`; the receiver repacks.
 *
 * **Bytes, not `AHardwareBuffer`, and deliberately so.** The background exists
 * to fill the *de-occlusion band*, which is thin — T0 shipped a 4x256 gradient
 * and it was visually sufficient everywhere except that the band needs *real*
 * content. SurfaceFlinger downscales for free (`DisplayCaptureArgs.setSize`),
 * so a 512x320 frame at <= 10 Hz is 640 KB and reuses T0's existing staging
 * upload verbatim — no JNI in the vendor APK, no cross-process image import, no
 * fence protocol. The zero-copy `AHardwareBuffer` + fence shape is specified as
 * the *product* transport in the AIDL (`ILeiaBackgroundCapture`), where the
 * producer is in-platform and the frame rate can matter.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Default abstract-socket name (without the leading NUL that marks it abstract).
#define COMP_MULTI_BG2D_CAPTURE_SOCKET "displayxr.bg2d"

//! Wire magics, little-endian u32 as written by the producer.
#define COMP_MULTI_BG2D_CAPTURE_MAGIC_HELLO 0x42525844u // 'DXRB'
#define COMP_MULTI_BG2D_CAPTURE_MAGIC_FRAME 0x46525844u // 'DXRF'
#define COMP_MULTI_BG2D_CAPTURE_VERSION 1u

/*!
 * One received frame, borrowed from the receiver.
 *
 * Valid only between a successful @ref comp_multi_bg2d_capture_acquire and the
 * matching @ref comp_multi_bg2d_capture_release — the receiver's lock is held
 * across that window, so copy or upload and get out.
 */
struct comp_multi_bg2d_capture_frame
{
	const uint8_t *pixels; //!< Tightly packed RGBA8, `width * height * 4` bytes.
	uint32_t width;
	uint32_t height;
	//! Receiver-side delivery counter, not the producer's wire sequence: it
	//! starts at 1 and never resets, so "nothing uploaded yet" is 0 and a
	//! producer restart cannot alias onto a frame the consumer already has.
	uint32_t seq;
};

/*!
 * Start the receiver if it is not already running. Idempotent and
 * process-global: one producer feeds every session, exactly as one screen has
 * one background.
 *
 * @param socket_name Abstract socket name, or NULL for the default.
 * @return false if the socket could not be created; the caller then behaves as
 *         if no background were configured.
 */
bool
comp_multi_bg2d_capture_start(const char *socket_name);

/*!
 * Borrow the most recent frame.
 *
 * @param[out] out         Filled on success.
 * @param      last_seq    Delivery counter the caller has already uploaded, 0
 *                         if none.
 * @return true when a frame newer than @p last_seq is available — and *only*
 *         then, so a steady producer at 10 Hz costs one upload per delivery,
 *         not one per compositor frame. The receiver lock is held on true and
 *         must be dropped with @ref comp_multi_bg2d_capture_release.
 */
bool
comp_multi_bg2d_capture_acquire(struct comp_multi_bg2d_capture_frame *out, uint32_t last_seq);

//! Release the borrow taken by a successful @ref comp_multi_bg2d_capture_acquire.
void
comp_multi_bg2d_capture_release(void);

//! Stop the receiver and free its buffers. Idempotent.
void
comp_multi_bg2d_capture_stop(void);

#ifdef __cplusplus
}
#endif
