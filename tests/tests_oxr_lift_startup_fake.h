// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Transport fake for tests_oxr_lift_startup: a scripted service under
 *         the real oxr_lift.c + ipc_client_lift.c, with a call trace.
 */

#pragma once

#include "xrt/xrt_openxr_includes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LF_TRACE_MAX 128
#define LF_TRACE_LINE 192

//! What the fake service exports for a result texture / its fence.
#define LF_OUTPUT_HANDLE 0x0BEE0
#define LF_FENCE_HANDLE 0x0FE40

enum lf_session_kind
{
	LF_SESSION_IPC,              //!< out-of-process: the session's own connection
	LF_SESSION_INPROCESS_D3D11,  //!< in-process, ADR-049 lift-only link
	LF_SESSION_INPROCESS_D3D12,  //!< in-process, ADR-049 lift-only link
	LF_SESSION_INPROCESS_GL,     //!< in-process, not eligible
};

//! Reset the trace, the scripted service and the link (CONNECTING, gen 0).
void
lf_reset(void);

size_t
lf_trace_count(void);

const char *
lf_trace_line(size_t i);

void
lf_trace_clear(void);

//! What lift_get_properties answers (XRT_DP_LIFT_STATE_*, XRT_DP_LIFT_MODE_* bits, ...).
void
lf_set_properties(uint32_t state, uint32_t modes, uint32_t max_views, uint32_t aux_outputs, uint32_t aux_depth_semantics);

//! xrt_result_t the next calls of that kind return (XRT_SUCCESS = 0 resets).
void
lf_set_properties_xret(int xret);
void
lf_set_create_xret(int xret);
void
lf_set_submit_xret(int xret);
void
lf_set_acquire_xret(int xret);

//! Whether lift_acquire_result has a result, and its shape.
void
lf_set_result_ready(bool ready, uint32_t width, uint32_t height, uint32_t view_count);

//! The NEXT lf_set_result_ready(true, ...) result says output_realloc.
void
lf_set_output_realloc_next(bool realloc);

//! The XrLiftOptionsDXR::viewCount the last lift_submit_frame carried on the wire.
uint32_t
lf_last_submit_view_count(void);

//! The ADR-049 link's state (enum ipc_client_lift_link_state) and generation.
void
lf_set_link(int state, uint32_t gen);

//! A live oxr_session of @p kind; XR_DXR_lift enabled on its instance iff @p lift_enabled.
XrSession
lf_session_create(enum lf_session_kind kind, bool lift_enabled);

bool
lf_session_lost(void);

//! oxr_handle_destroy() the session (and so its lift streams), then free it.
void
lf_session_destroy(void);

#ifdef __cplusplus
}
#endif
