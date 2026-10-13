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
 *  lift-only connection (D3D11 / D3D12 on Windows). */
bool
oxr_conv_inprocess_eligible(struct oxr_session *sess);

/*! The session's lift link, created on first use (no connect yet). NULL off Windows. */
struct ipc_client_lift_link *
oxr_conv_link_get(struct oxr_session *sess);

/*! XrLiftImageKindDXR masks (1 << kind) this session can hand in / take out. */
void
oxr_conv_session_image_kinds(struct oxr_session *sess, uint64_t *out_in_kinds, uint64_t *out_export_kinds);

#ifdef __cplusplus
}
#endif
