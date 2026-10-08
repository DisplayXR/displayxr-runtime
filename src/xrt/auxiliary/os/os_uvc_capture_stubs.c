// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  No UVC capture backend on this platform (yet) — see os_uvc_capture.h.
 *
 * TODO(linux): a V4L2 backend for desktop Linux (XRT_OS_LINUX_DESKTOP):
 * enumerate /dev/video* with VIDIOC_QUERYCAP (V4L2_CAP_VIDEO_CAPTURE, skipping
 * metadata nodes) and the USB VID:PID from sysfs
 * (/sys/class/video4linux/videoN/device/../idVendor, idProduct); modes from
 * VIDIOC_ENUM_FMT / ENUM_FRAMESIZES / ENUM_FRAMEINTERVALS; open = S_FMT + S_PARM
 * + mmap'd buffers + STREAMON on a reader thread keeping the newest buffer;
 * YUYV is handed over as U_STEREO_UVC_PIXEL_YUY2, MJPEG needs a decoder (libjpeg-
 * turbo, dlopen'd so the runtime keeps no hard dependency). Until then a Linux
 * service sees only the config's fake device.
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
		snprintf(out, cap, "no UVC capture backend on this platform (Linux V4L2 is a TODO)\n");
	}
}
