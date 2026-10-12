// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  C11 translation unit for tests_conv_wire: xrt_conversion_plugin.h and
 *         xrt_conv_wire.h must compile as C (every _Static_assert included), and
 *         the inline helpers must behave the same from C.
 */

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_conversion_plugin.h"

#include <string.h>

int
tests_conv_wire_c_roundtrip(void);

int
tests_conv_wire_c_roundtrip(void)
{
	struct xrt_conv_depth_info a;
	struct xrt_conv_depth_info b;
	struct xrt_conv_wire_result w;
	struct xrt_conv_wire_result r;
	struct xrt_conv_frame_out fo;
	struct xrt_conv_frame_out back;
	unsigned char *p = (unsigned char *)&a;
	size_t i;
	uint64_t frame_id = 0;

	for (i = 0; i < sizeof(a); i++) {
		p[i] = (unsigned char)(((i * 29u) + 3u) % 63u + 1u);
	}
	memset(&b, 0, sizeof(b));
	memset(&fo, 0, sizeof(fo));
	fo.struct_size = (uint32_t)sizeof(fo);
	fo.written = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
	fo.depth_info = a;

	xrt_conv_wire_pack_result(7u, 42u, 1000u, 5u, 1u, &fo, &w);
	if (!xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &w, w.struct_size)) {
		return 1;
	}
	xrt_conv_wire_unpack_result(&r, NULL, &frame_id, NULL, NULL, NULL, &back);
	if (frame_id != 42u || back.written != fo.written) {
		return 2;
	}
	b = back.depth_info;
	if (memcmp(&a, &b, sizeof(a)) != 0) {
		return 3;
	}
	return 0;
}
