// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The POSIX CPU I/O glue of the conversion core (ipc_server_conv_cpu.c)
 *         and the CPU buffer it rides (ipc_cpu_buffer.c): XR_DXR_lift's CPU
 *         kind on desktop Linux / macOS, with no IPC and no provider.
 */

#include "catch_amalgamated.hpp"

#include "server/ipc_server_conv.h"
#include "server/ipc_server_conv_cpu.h"
#include "shared/ipc_cpu_buffer.h"

#include "util/u_handles.h"

#include <cstring>
#include <vector>

namespace {

//! A tightly packed RGBA8 section holding @p px, as the client would hand it in.
xrt_graphics_buffer_handle_t
upload(const std::vector<uint8_t> &px)
{
	xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(ipc_cpu_buffer_create(px.size(), &h));
	struct ipc_cpu_buffer_map m;
	REQUIRE(ipc_cpu_buffer_map(h, px.size(), true, &m));
	memcpy(m.ptr, px.data(), px.size());
	ipc_cpu_buffer_unmap(&m);
	return h;
}

} // namespace

TEST_CASE("ipc_cpu_buffer: create, map, refuse an oversize map", "[conv][cpu_io]")
{
	if (!ipc_cpu_buffer_supported()) {
		SKIP("no CPU buffer transport on this platform");
	}
	xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(ipc_cpu_buffer_create(10000, &h));
	struct ipc_cpu_buffer_map w;
	REQUIRE(ipc_cpu_buffer_map(h, 10000, true, &w));
	static_cast<uint8_t *>(w.ptr)[9999] = 0x5a;
	struct ipc_cpu_buffer_map r;
	REQUIRE(ipc_cpu_buffer_map(h, 10000, false, &r));
	CHECK(static_cast<const uint8_t *>(r.ptr)[9999] == 0x5a); // one section, two views
	ipc_cpu_buffer_unmap(&r);
	ipc_cpu_buffer_unmap(&w);
	// The wire is not trusted: a section shorter than the claimed bytes is refused.
	CHECK_FALSE(ipc_cpu_buffer_map(h, 64ull * 1024 * 1024, false, &r));
	u_graphics_buffer_unref(&h);
	CHECK_FALSE(xrt_graphics_buffer_is_valid(h));
}

TEST_CASE("cpu io glue: CPU in -> slot, result -> export section; no v3, no D3D", "[conv][cpu_io]")
{
	ipc_server_conv_io_ops io;
	if (!ipc_server_conv_cpu_io_get(&io)) {
		SKIP("no CPU buffer transport on this platform");
	}
	CHECK(io.cpu_only);
	REQUIRE(io.import_input_ex != nullptr);
	REQUIRE(io.export_cpu != nullptr);

	// v3 entry points refuse (and still take the handle).
	xrt_graphics_buffer_handle_t dummy = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	REQUIRE(ipc_cpu_buffer_create(16, &dummy));
	CHECK(io.import_input(io.ud, 1, dummy, false, 2, 2, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	xrt_graphics_buffer_handle_t th;
	CHECK_FALSE(io.export_output(io.ud, 1, false, &th, nullptr, nullptr, nullptr));

	// A 6x4 BGRA source whose region (1,1 4x2) lands in a 4x2 RGBA slot image.
	const uint32_t W = 6, H = 4;
	std::vector<uint8_t> src((size_t)W * H * 4);
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *p = &src[((size_t)y * W + x) * 4];
			p[0] = (uint8_t)(10 * x); // B
			p[1] = (uint8_t)(10 * y); // G
			p[2] = 200;               // R
			p[3] = 255;
		}
	}
	std::vector<uint8_t> slot_px(4 * 2 * 4, 0);
	conv_submit_slot slot{};
	slot.image_count = 1;
	xrt_conv_image &si = slot.images[0];
	si.memory_kind = XRT_CONV_MEM_CPU;
	si.cpu_ptr = slot_px.data();
	si.width = 4;
	si.height = 2;
	si.format = XRT_CONV_FMT_RGBA8_UNORM;
	si.row_stride = 16;
	si.byte_size = slot_px.size();

	conv_input_import in{};
	in.kind = CONV_INPUT_CPU;
	in.handle = upload(src);
	in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	in.x = 1;
	in.y = 1;
	in.w = 4;
	in.h = 2;
	in.cpu_row_pitch = W * 4;
	in.cpu_bgra = true;
	REQUIRE(io.import_input_ex(io.ud, 7, &in, &slot) == XRT_SUCCESS);
	// Slot texel (0,0) = source (1,1), swizzled to RGBA.
	CHECK(slot_px[0] == 200);
	CHECK(slot_px[1] == 10);
	CHECK(slot_px[2] == 10);
	// Slot texel (3,1) = source (4,2).
	CHECK(slot_px[(1 * 4 + 3) * 4 + 2] == 40);
	CHECK(slot_px[(1 * 4 + 3) * 4 + 1] == 20);

	// A region past the section's end is refused (the handle is still taken).
	// Far past: a macOS section is rounded up to whole 4 KiB rows.
	in.handle = upload(src);
	in.h = 4000;
	CHECK(io.import_input_ex(io.ud, 7, &in, &slot) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);

	// Publish a 2-view 8x2 RGBA result with a padded row stride.
	std::vector<uint8_t> views((size_t)40 * 2, 0);
	for (uint32_t y = 0; y < 2; y++) {
		for (uint32_t x = 0; x < 8; x++) {
			views[(size_t)y * 40 + x * 4] = (uint8_t)(x + 8 * y);
		}
	}
	conv_result res{};
	res.written = XRT_CONV_OUT_VIEWS;
	res.views.memory_kind = XRT_CONV_MEM_CPU;
	res.views.cpu_ptr = views.data();
	res.views.width = 8;
	res.views.height = 2;
	res.views.format = XRT_CONV_FMT_RGBA8_UNORM;
	res.views.row_stride = 40;
	res.views.byte_size = views.size();
	ipc_server_conv_published pub{};
	REQUIRE(io.publish_result(io.ud, 7, &res, &pub) == XRT_SUCCESS);
	CHECK(pub.width == 8);
	CHECK(pub.height == 2);
	CHECK(pub.format == 28u); // the wire's DXGI_FORMAT_R8G8B8A8_UNORM
	CHECK(pub.realloc);
	REQUIRE(io.publish_result(io.ud, 7, &res, &pub) == XRT_SUCCESS);
	CHECK_FALSE(pub.realloc); // same shape: same section

	xrt_graphics_buffer_handle_t eh = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	uint32_t w = 0, h = 0, f = 0;
	uint64_t bytes = 0;
	bool realloc = true;
	REQUIRE(io.export_cpu(io.ud, 7, false, &eh, &w, &h, &f, &bytes, &realloc));
	CHECK(bytes == 8u * 2u * 4u); // tightly packed
	struct ipc_cpu_buffer_map m;
	REQUIRE(ipc_cpu_buffer_map(eh, bytes, false, &m)); // the glue keeps eh
	const auto *e = static_cast<const uint8_t *>(m.ptr);
	CHECK(e[0] == 0);
	CHECK(e[7 * 4] == 7);
	CHECK(e[(8 + 5) * 4] == 13); // row 1 starts at 32 bytes, not 40
	ipc_cpu_buffer_unmap(&m);

	io.stream_gone(io.ud, 7);
	CHECK_FALSE(io.export_cpu(io.ud, 7, false, &eh, &w, &h, &f, &bytes, &realloc));
}
