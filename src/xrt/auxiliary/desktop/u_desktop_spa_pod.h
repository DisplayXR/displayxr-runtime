// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The few SPA pods the GNOME screen-capture backend exchanges with
 *         PipeWire, built and parsed by hand (XR_DXR_desktop_control spec §10.5).
 *
 * libpipewire-0.3 is dlopen'd (no DT_NEEDED, no build dependency on its
 * headers), and SPA pods are header-only inline code in libspa, so the two pods
 * a video capture stream needs are written here against the stable SPA wire
 * format (spa/pod/pod.h: an 8-byte header {size, type}, bodies padded to 8):
 *
 *   EnumFormat  video/raw, format one of BGRx BGRA RGBx RGBA, any size, any rate
 *   Buffers     dataType MemPtr | MemFd (SHM only: no DMA-BUF import, no GPU)
 *
 * and the negotiated Format is parsed back for its size and pixel format. Pure C,
 * no OS calls: compiled and unit tested on every OS; the bytes were checked
 * against libspa's own builder (spa_pod_builder / spa_format_video_raw_build).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// spa/utils/type.h, spa/param/param.h, spa/param/format.h, spa/param/video/raw.h,
// spa/param/buffers.h, spa/buffer/buffer.h — the values this file needs.
#define U_SPA_TYPE_Id 3
#define U_SPA_TYPE_Int 4
#define U_SPA_TYPE_Rectangle 10
#define U_SPA_TYPE_Fraction 11
#define U_SPA_TYPE_Object 15
#define U_SPA_TYPE_Choice 19
#define U_SPA_TYPE_OBJECT_Format 0x40003
#define U_SPA_TYPE_OBJECT_ParamBuffers 0x40004
#define U_SPA_PARAM_EnumFormat 3
#define U_SPA_PARAM_Format 4
#define U_SPA_PARAM_Buffers 5
#define U_SPA_CHOICE_None 0
#define U_SPA_CHOICE_Range 1
#define U_SPA_CHOICE_Enum 3
#define U_SPA_CHOICE_Flags 4
#define U_SPA_FORMAT_mediaType 1
#define U_SPA_FORMAT_mediaSubtype 2
#define U_SPA_FORMAT_VIDEO_format 0x20001
#define U_SPA_FORMAT_VIDEO_size 0x20003
#define U_SPA_FORMAT_VIDEO_framerate 0x20004
#define U_SPA_MEDIA_TYPE_video 2
#define U_SPA_MEDIA_SUBTYPE_raw 1
#define U_SPA_VIDEO_FORMAT_RGBx 7
#define U_SPA_VIDEO_FORMAT_BGRx 8
#define U_SPA_VIDEO_FORMAT_RGBA 11
#define U_SPA_VIDEO_FORMAT_BGRA 12
#define U_SPA_PARAM_BUFFERS_dataType 6
#define U_SPA_DATA_MemPtr 1
#define U_SPA_DATA_MemFd 2
#define U_SPA_DATA_DmaBuf 3
#define U_SPA_CHUNK_FLAG_CORRUPTED 1

//! Room for either pod this file builds.
#define U_SPA_POD_BUF 512

/*!
 * The EnumFormat pod a capture stream offers. Writes into @p buf (8-aligned,
 * at least U_SPA_POD_BUF bytes); returns its size, 0 if it does not fit.
 */
size_t
u_spa_build_enum_format(uint8_t *buf, size_t cap);

//! The Buffers pod (dataType MemPtr | MemFd). As above.
size_t
u_spa_build_buffers(uint8_t *buf, size_t cap);

/*!
 * Parse a negotiated Format pod (@p pod, @p len bytes incl. its header): the video
 * size and the SPA video format. False if it is not a video Format object with a
 * size and a format (a Choice is read as its default value).
 */
bool
u_spa_parse_video_format(const void *pod, size_t len, uint32_t *w, uint32_t *h, uint32_t *format);

#ifdef __cplusplus
}
#endif
