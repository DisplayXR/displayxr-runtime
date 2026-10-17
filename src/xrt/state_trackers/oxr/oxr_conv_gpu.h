// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift: graphics-API / platform seam of the conversion entry
 *         points (oxr_conv_gpu_d3d.c). See that file.
 * @ingroup oxr_api
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct oxr_session;
struct ipc_client_lift_link;

/*! ADR-049: an in-process session that reaches the conversion service over a
 *  lift-only connection — any in-process native compositor on Windows,
 *  desktop Linux and macOS (the spec-4 calls; CPU kinds off D3D). */
bool
oxr_conv_inprocess_eligible(struct oxr_session *sess);

/*! The in-process sessions the spec-3 calls serve: D3D11 / D3D12 on Windows
 *  (spec 3 is D3D11 shared textures by definition). */
bool
oxr_conv_v3_eligible(struct oxr_session *sess);

/*! The session's lift link, created on first use (no connect yet). NULL where
 *  no lift link exists (Android). */
struct ipc_client_lift_link *
oxr_conv_link_get(struct oxr_session *sess);

/*! XrLiftImageKindDXR masks (1 << kind) this session can hand in / take out. */
void
oxr_conv_session_image_kinds(struct oxr_session *sess, uint64_t *out_in_kinds, uint64_t *out_export_kinds);

/*! CPU input: copy the region (@p x, @p y, @p w x @p h of @p bytes, @p pitch)
 *  tightly packed into a new shared section (Windows: a file mapping; POSIX:
 *  an IPC CPU buffer); *out_handle is the caller's. */
bool
oxr_conv_cpu_upload(const void *bytes, uint32_t pitch, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void **out_handle);

/*! CPU export: a read-only view of a mapping handle (NULL on failure);
 *  *out_token goes back to oxr_conv_cpu_unmap with the view. Does not take
 *  @p handle (the view keeps the section alive). */
void *
oxr_conv_cpu_map(void *handle, uint64_t bytes, void **out_token);

void
oxr_conv_cpu_unmap(void *view, void *token);

void
oxr_conv_close_handle(void *handle);

#ifdef __cplusplus
}
#endif
