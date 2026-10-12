// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS display-reconfiguration notification —
 *         `CGDisplayRegisterReconfigurationCallback`.
 * @ingroup aux_os
 */

#include "os_display_hotplug.h"

#include <CoreGraphics/CoreGraphics.h>

#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static os_display_hotplug_fn g_fn = NULL;
static void *g_user = NULL;
static bool g_registered = false;

static void
reconfigured(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *user_info)
{
	(void)display;
	(void)user_info;
	// Each change arrives twice: once before (BeginConfiguration) and once
	// after. Only the second describes the new topology.
	if ((flags & kCGDisplayBeginConfigurationFlag) != 0) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	os_display_hotplug_fn fn = g_fn;
	void *user = g_user;
	if (fn != NULL) {
		fn(user);
	}
	pthread_mutex_unlock(&g_lock);
}

bool
os_display_hotplug_subscribe(os_display_hotplug_fn fn, void *user)
{
	pthread_mutex_lock(&g_lock);
	g_fn = fn;
	g_user = user;
	bool ok = true;
	if (!g_registered) {
		ok = CGDisplayRegisterReconfigurationCallback(reconfigured, NULL) == kCGErrorSuccess;
		g_registered = ok;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

void
os_display_hotplug_unsubscribe(void)
{
	// The CoreGraphics registration stays (removing it from another thread
	// than the one delivering would race a callback in flight); clearing the
	// subscriber under the lock the callback takes is what guarantees no call
	// after this returns.
	pthread_mutex_lock(&g_lock);
	g_fn = NULL;
	g_user = NULL;
	pthread_mutex_unlock(&g_lock);
}
