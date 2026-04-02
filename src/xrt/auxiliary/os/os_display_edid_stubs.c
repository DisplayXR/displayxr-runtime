// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stub implementation of EDID display enumeration for non-Windows.
 * @ingroup aux_os
 */

#include "os_display_edid.h"
#include <string.h>

bool
os_display_edid_enumerate(struct os_display_edid_list *out_list)
{
	if (out_list == NULL) {
		return false;
	}
	memset(out_list, 0, sizeof(*out_list));
	return false;
}

const struct os_display_edid_monitor *
os_display_edid_find_in_table(const struct os_display_edid_list *list,
                              const uint16_t table[][2],
                              uint32_t table_len)
{
	(void)list;
	(void)table;
	(void)table_len;
	return NULL;
}
