// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  No UVC capture backend on this platform — see os_uvc_capture.h.
 *
 * Windows has Media Foundation (os_uvc_capture_mf.cpp), desktop Linux V4L2
 * (os_uvc_capture_v4l2.c); macOS and Android have none, so the source there
 * only ever sees the config's fake device.
 *
 * @ingroup aux_os
 */

#include "os/os_uvc_capture.h"

#include <stdio.h>
#include <string.h>

bool
os_uvc_capture_backend(struct u_stereo_uvc_backend *out)
{
	memset(out, 0, sizeof(*out));
	return false;
}

void
os_uvc_capture_decoder_report(char *out, size_t cap)
{
	if (out != NULL && cap > 0) {
		snprintf(out, cap, "no UVC capture backend on this platform\n");
	}
}
