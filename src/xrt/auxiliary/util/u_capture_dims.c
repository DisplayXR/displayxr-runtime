// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Process-global capture-dims provider registry (see u_capture_dims.h).
 * @ingroup aux_util
 */

#include "util/u_capture_dims.h"

#include <pthread.h>

// A single installed provider — matches the in-process single-compositor model
// the rest of the capture machinery (mcp_capture) already assumes. Guarded by a
// statically-initialized mutex so register/clear/query never tear.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static u_capture_dims_fn g_fn = NULL;
static void *g_userdata = NULL;

void
u_capture_dims_set_provider(u_capture_dims_fn fn, void *userdata)
{
	pthread_mutex_lock(&g_lock);
	if (fn == NULL && g_userdata != userdata) {
		// Clear request from a caller that no longer owns the slot — ignore.
		pthread_mutex_unlock(&g_lock);
		return;
	}
	g_fn = fn;
	g_userdata = fn != NULL ? userdata : NULL;
	pthread_mutex_unlock(&g_lock);
}

bool
u_capture_dims_query(uint32_t *out_view_w,
                     uint32_t *out_view_h,
                     uint32_t *out_tile_cols,
                     uint32_t *out_tile_rows)
{
	pthread_mutex_lock(&g_lock);
	u_capture_dims_fn fn = g_fn;
	void *ud = g_userdata;
	pthread_mutex_unlock(&g_lock);

	if (fn == NULL) {
		return false;
	}
	return fn(ud, out_view_w, out_view_h, out_tile_cols, out_tile_rows);
}
