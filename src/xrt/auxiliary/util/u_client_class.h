// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure rules about IPC client classes (#960, ADR-035 D1/D6) that the
 *         service applies at admission, kept free of server state so they can
 *         be tested host-side.
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One already-admitted connection, as the PRESENT_OWNER count sees it.
struct u_client_class_peer
{
	uint32_t client_class; //!< enum xrt_client_class (the VERIFIED class)
	const char *exe;       //!< OS-derived image path; "" when the OS would not reveal it
};

/*!
 * Does a connection of this class hold the panel as an owner? Only
 * PRESENT_OWNER does: a CAMERA_CONSUMER from the browser's executable (its
 * video-capture utility, ADR-043 R3) reads a camera, it never presents.
 */
bool
u_client_class_counts_toward_present_owner(uint32_t client_class);

/*!
 * The PRESENT_OWNER quota counts OWNERS (distinct executables), not
 * connections. Returns the number of distinct owner executables among
 * @p peers (only PRESENT_OWNER peers count), or 0 when @p candidate_exe is a
 * SIBLING of one of them — a second process of an already-admitted owner takes
 * no new slot. A peer whose path is unknown ("") is its own owner and is never
 * anyone's sibling (fails closed); an unknown candidate is never a sibling.
 */
uint32_t
u_client_class_present_owner_count(const struct u_client_class_peer *peers, uint32_t n, const char *candidate_exe);

/*!
 * May a connection of this class create a session / compositor? RELAY may
 * not create a native compositor (its own rule in the handler); a
 * CAMERA_CONSUMER may not create a session at all.
 */
bool
u_client_class_may_create_session(uint32_t client_class);

#ifdef __cplusplus
}
#endif
