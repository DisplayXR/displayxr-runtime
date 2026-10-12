// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift caps <-> lift_get_properties wire struct, the ONE copy
 *         both ends use.
 *
 * The service filled `ipc_lift_properties` from `xrt_dp_lift_caps` field by
 * field and the client copied it back field by field, by hand, in two files.
 * The client copy missed `aux_outputs` / `aux_depth_semantics` (#2005 G1,
 * conversion-providers roadmap §10), so `XrLiftDepthPropertiesDXR::
 * auxDepthSupported` read false through every IPC session and the ADR-049
 * lift-only link. Both directions now live here, next to each other, and
 * tests_oxr_lift_startup round-trips every field through them.
 *
 * The wire layout (`struct ipc_lift_properties`) is unchanged.
 *
 * @ingroup ipc_shared
 */

#pragma once

#include "xrt/xrt_dp_lift.h"

#include "shared/ipc_protocol.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Copy a fixed-size string field, always NUL-terminated.
#define IPC_LIFT_CAPS_COPY_STR(dst, src)                                                                                \
	do {                                                                                                           \
		memcpy((dst), (src), sizeof(dst) < sizeof(src) ? sizeof(dst) : sizeof(src));                         \
		(dst)[sizeof(dst) - 1] = '\0';                                                                         \
	} while (0)

/*!
 * Service side: the module's caps -> the lift_get_properties reply.
 * @p out is fully overwritten (reserved fields zero).
 */
static inline void
ipc_lift_properties_from_caps(const struct xrt_dp_lift_caps *caps, struct ipc_lift_properties *out)
{
	memset(out, 0, sizeof(*out));
	out->modes = caps->modes;
	out->max_streams = caps->max_streams;
	out->max_views = caps->max_views;
	out->depth_semantics = caps->depth_semantics;
	out->state = caps->state;
	out->aux_outputs = caps->aux_outputs;
	out->typical_latency_ns = caps->typical_latency_ns;
	IPC_LIFT_CAPS_COPY_STR(out->backend, caps->backend);
	out->aux_depth_semantics = caps->aux_depth_semantics;
	out->module_status = caps->module_status;
	IPC_LIFT_CAPS_COPY_STR(out->module_version, caps->module_version);
	IPC_LIFT_CAPS_COPY_STR(out->module_detail, caps->module_detail);
}

/*!
 * Client side: the lift_get_properties reply -> caps. @p out is re-initialised
 * (xrt_dp_lift_caps_init: zeroed, struct_size stamped) before the copy.
 */
static inline void
ipc_lift_caps_from_properties(const struct ipc_lift_properties *props, struct xrt_dp_lift_caps *out)
{
	xrt_dp_lift_caps_init(out);
	out->modes = props->modes;
	out->max_streams = props->max_streams;
	out->max_views = props->max_views;
	out->depth_semantics = props->depth_semantics;
	out->state = props->state;
	out->typical_latency_ns = props->typical_latency_ns;
	IPC_LIFT_CAPS_COPY_STR(out->backend, props->backend);
	out->aux_outputs = props->aux_outputs;
	out->aux_depth_semantics = props->aux_depth_semantics;
	out->module_status = props->module_status;
	IPC_LIFT_CAPS_COPY_STR(out->module_version, props->module_version);
	IPC_LIFT_CAPS_COPY_STR(out->module_detail, props->module_detail);
}

#ifdef __cplusplus
}
#endif
