// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure client-class rules (see u_client_class.h).
 * @ingroup aux_util
 */

#include "util/u_client_class.h"

#include "xrt/xrt_instance.h"

#include <string.h>

bool
u_client_class_counts_toward_present_owner(uint32_t client_class)
{
	return client_class == XRT_CLIENT_CLASS_PRESENT_OWNER;
}

bool
u_client_class_may_create_session(uint32_t client_class)
{
	return client_class != XRT_CLIENT_CLASS_CAMERA_CONSUMER;
}

uint32_t
u_client_class_present_owner_count(const struct u_client_class_peer *peers, uint32_t n, const char *candidate_exe)
{
	const char *seen[64];
	uint32_t distinct = 0;
	const bool candidate_known = candidate_exe != NULL && candidate_exe[0] != '\0';
	for (uint32_t i = 0; i < n; i++) {
		const struct u_client_class_peer *p = &peers[i];
		if (!u_client_class_counts_toward_present_owner(p->client_class)) {
			continue;
		}
		const char *exe = p->exe != NULL ? p->exe : "";
		if (exe[0] != '\0' && candidate_known && strcmp(exe, candidate_exe) == 0) {
			return 0; // a sibling of an admitted owner: no new slot
		}
		bool dup = false;
		for (uint32_t k = 0; k < distinct && exe[0] != '\0'; k++) {
			if (strcmp(seen[k], exe) == 0) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			if (distinct < sizeof(seen) / sizeof(seen[0])) {
				seen[distinct] = exe;
			}
			distinct++;
		}
	}
	return distinct;
}
