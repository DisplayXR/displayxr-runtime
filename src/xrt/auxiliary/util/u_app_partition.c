// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Process-wide outcome state of the vblank slot partition (#1442).
 *
 * u_app_partition.h is header-only and its function-local statics are
 * per translation unit; the engaged/refused OUTCOME must be one value for
 * the whole process because its consumer (the late-weave governor, in the
 * target) and its producer (the throttle, in the compositor) live in
 * different translation units.
 * @ingroup aux_util
 */

#include "util/u_app_partition.h"

static enum u_app_partition_state g_state = U_APP_PARTITION_UNKNOWN;

void
u_app_partition_set_state(enum u_app_partition_state state)
{
	g_state = state;
}

enum u_app_partition_state
u_app_partition_state(void)
{
	return g_state;
}
