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
 * different translation units — and on different threads (written in
 * xrWaitFrame, read on the weave path), hence the atomic accessors.
 * @ingroup aux_util
 */

#include "util/u_app_partition.h"

#ifdef _WIN32
#include <windows.h>
static volatile LONG g_state = U_APP_PARTITION_UNKNOWN;
#else
static int g_state = U_APP_PARTITION_UNKNOWN;
#endif

void
u_app_partition_set_state(enum u_app_partition_state state)
{
#ifdef _WIN32
	InterlockedExchange(&g_state, (LONG)state);
#else
	__atomic_store_n(&g_state, (int)state, __ATOMIC_RELEASE);
#endif
}

enum u_app_partition_state
u_app_partition_state(void)
{
#ifdef _WIN32
	return (enum u_app_partition_state)InterlockedCompareExchange(&g_state, 0, 0);
#else
	return (enum u_app_partition_state)__atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
#endif
}
