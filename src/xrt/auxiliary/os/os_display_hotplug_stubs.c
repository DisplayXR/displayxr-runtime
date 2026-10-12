// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-reconfiguration notification: not implemented on this
 *         platform; the runtime polls the topology signature instead.
 * @ingroup aux_os
 *
 * TODO(display hotplug, ADR-047 Amendment 8): Windows WM_DISPLAYCHANGE on a
 * message-only window; Linux RandR RRScreenChangeNotify / XCB
 * randr_screen_change_notify (or a udev drm "change" uevent).
 */

#include "os_display_hotplug.h"

#include <stddef.h>

bool
os_display_hotplug_subscribe(os_display_hotplug_fn fn, void *user)
{
	(void)fn;
	(void)user;
	return false;
}

void
os_display_hotplug_unsubscribe(void)
{}
