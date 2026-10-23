// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Topology read: none on this platform (macOS: the desktop
 *         enumeration is the signature; Android: no hotplug sweep).
 * @ingroup aux_os
 */

#include "os_display_topology.h"

#include <stddef.h>

uint32_t
os_display_topology_read(struct os_display_topology_entry *out, uint32_t max)
{
	(void)out;
	(void)max;
	return 0;
}
