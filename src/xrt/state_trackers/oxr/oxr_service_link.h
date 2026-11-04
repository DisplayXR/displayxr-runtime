// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: an in-process session's service link at session
 *         begin / end (see oxr_service_link.c).
 * @ingroup oxr_main
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct oxr_session;

//! xrBeginSession: open + register the link (sharing on), install the share sink.
void
oxr_service_link_session_begin(struct oxr_session *sess);

//! xrEndSession: re-register "not running", remove the share sink.
void
oxr_service_link_session_end(struct oxr_session *sess);

//! Session destroy, BEFORE the link is freed: remove the share sink.
void
oxr_service_link_session_fini(struct oxr_session *sess);

#ifdef __cplusplus
}
#endif
