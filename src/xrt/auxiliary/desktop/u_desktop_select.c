// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-control backend selection (XR_DXR_desktop_control, ADR-056).
 *
 * `DXR_DESKTOP_BACKEND=fake` selects the in-memory fake (u_desktop_fake.h) so
 * the whole pipeline runs in CI and on a developer box without touching the
 * real desktop; `none` selects nothing (UNSUPPORTED). Anything else: the one
 * platform backend compiled into this build.
 */

#include "u_desktop_control.h"
#include "u_desktop_fake.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

struct u_desktop_backend *
u_desktop_backend_create(void)
{
	const char *sel = getenv("DXR_DESKTOP_BACKEND");
	if (sel != NULL && strcmp(sel, "fake") == 0) {
		U_LOG_W("desktop_control: DXR_DESKTOP_BACKEND=fake - using the in-memory test desktop");
		return u_desktop_fake_create();
	}
	if (sel != NULL && strcmp(sel, "none") == 0) {
		return NULL;
	}
	return u_desktop_backend_create_platform();
}
