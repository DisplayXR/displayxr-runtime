// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared fixture of the spec-4 service-side tests (#2054):
 *         tests_ipc_conv_v4.cpp and tests_lift_v3_shifts.cpp. A conversion
 *         core + the conv_* adapter, a CPU GPU-glue stand-in, the CPU fake
 *         provider, and core events forwarded to the adapter as the service
 *         wires them.
 */

#pragma once

#include "catch_amalgamated.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "conversion/conv_readiness.h"

#include "server/ipc_server_conv.h"
#include "shared/ipc_protocol.h"

#include "xrt/xrt_conv_wire.h"

#include "os/os_time.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t MS = 1000000ull;

inline void
sleep_ms(uint32_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = os_monotonic_get_ns() + (uint64_t)timeout_ms * MS;
	while (os_monotonic_get_ns() < end) {
		if (pred()) {
			return true;
		}
		sleep_ms(2);
	}
	return pred();
}

inline bool
cpu_alloc(void *, const conv_image_request *req, xrt_conv_image *out)
{
	const uint32_t bpp = req->format == XRT_CONV_FMT_R8_UNORM ? 1 : 4;
	const uint64_t size = (uint64_t)req->width * req->height * bpp * (req->array_layers ? req->array_layers : 1);
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	static std::atomic<uint64_t> key{1};
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp;
	out->import_key = key++;
	return true;
}

inline void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
}

struct Glue
{
	std::mutex m;
	std::map<uint64_t, uint32_t> export_w;
	uint64_t fence = 0;
};

inline xrt_result_t
g_import(void *, uint64_t, xrt_graphics_buffer_handle_t, bool, uint32_t, uint32_t, const conv_submit_slot *slot)
{
	if (slot->image_count < 1 || slot->images[0].cpu_ptr == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	std::memset(slot->images[0].cpu_ptr, 0x80, (size_t)slot->images[0].byte_size);
	return XRT_SUCCESS;
}

inline xrt_result_t
g_import_cpu(void *, uint64_t, xrt_graphics_buffer_handle_t, bool, uint32_t w, uint32_t h, uint8_t **out)
{
	*out = (uint8_t *)std::calloc((size_t)w * h, 4);
	return *out != nullptr ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
}

inline void
g_release(void *, xrt_graphics_buffer_handle_t, bool)
{}

inline xrt_result_t
g_publish(void *ud, uint64_t stream_id, const conv_result *r, ipc_server_conv_published *out)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	const xrt_conv_image &src = (r->written & XRT_CONV_OUT_VIEWS) != 0 ? r->views : r->depth;
	uint32_t &w = g->export_w[stream_id];
	out->realloc = w != src.width;
	w = src.width;
	out->width = src.width;
	out->height = src.height;
	out->format = 28;
	out->fence_value = ++g->fence;
	out->depth_width = r->depth.width;
	out->depth_height = r->depth.height;
	out->depth_format = 41;
	return XRT_SUCCESS;
}

inline bool
g_export(void *, uint64_t, bool, xrt_graphics_buffer_handle_t *, uint32_t *, uint32_t *, uint32_t *)
{
	return false;
}

inline bool
g_fence(void *, uint64_t, xrt_graphics_sync_handle_t *)
{
	return false;
}

inline void
g_gone(void *ud, uint64_t stream_id)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	g->export_w.erase(stream_id);
}

inline void
fwd_event(void *ud, uint64_t client_id, const xrt_conv_wire_event *ev)
{
	ipc_server_conv_on_event(*static_cast<ipc_server_conv **>(ud), client_id, ev);
}

struct Fixture
{
	conv_core *core = nullptr;
	ipc_server_conv *sc = nullptr;
	Glue glue;

	explicit Fixture(uint32_t prewarm_on = CONV_PREWARM_FIRST_CREATE)
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = prewarm_on;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		cfg.on_event = fwd_event;
		cfg.event_ud = &sc; // set below, before any provider exists
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		REQUIRE(ipc_server_conv_create(core, &sc) == XRT_SUCCESS);
		ipc_server_set_conv(sc);
	}
	~Fixture()
	{
		ipc_server_set_conv(nullptr);
		ipc_server_conv *dead = sc;
		ipc_server_conv_destroy(&dead);
		sc = nullptr;
		conv_core_destroy(&core);
	}

	void
	install_glue(bool cpu = false)
	{
		ipc_server_conv_io_ops io{};
		io.import_input = g_import;
		io.release_input = g_release;
		io.publish_result = g_publish;
		io.export_output = g_export;
		io.export_fence = g_fence;
		io.stream_gone = g_gone;
		io.import_input_cpu = cpu ? g_import_cpu : nullptr;
		io.ud = &glue;
		ipc_server_conv_set_io(sc, &io);
	}

	int32_t
	add(conv_fake_provider &f, uint32_t probe_order = 100)
	{
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = probe_order;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		const int32_t idx = conv_core_add_provider(core, &d);
		// Every test here starts from a described provider (cold boot is
		// tests_ipc_lift_core's G5 case).
		REQUIRE(wait_until([&] { return conv_core_discovery_settled(core); }, 5000));
		return idx;
	}

	xrt_result_t
	create_task(uint64_t client,
	            uint32_t task,
	            uint64_t *id,
	            uint32_t view_count = 0,
	            uint64_t required = 0,
	            uint32_t mode = 0,
	            uint32_t input_kind = XRT_CONV_INPUT_MONO,
	            uint32_t export_kind = 0)
	{
		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = task;
		sd.input_kind = input_kind;
		sd.view_count = view_count;
		sd.export_memory_kind = export_kind;
		xrt_conv_wire_stream_created c{};
		xrt_result_t r = ipc_server_conv_v4_stream_create(sc, client, &sd, required, mode, 1.0f, &c);
		*id = c.stream_id;
		return r;
	}

	xrt_result_t
	submit(uint64_t client, uint64_t id)
	{
		ipc_arg_lift_submit a{};
		a.stream_id = id;
		a.source_time = 42;
		a.width = 64;
		a.height = 32;
		uint64_t fid = 0;
		return ipc_server_conv_lift_submit(sc, client, &a, (xrt_graphics_buffer_handle_t)(intptr_t)0x1234,
		                                   false, &fid);
	}

	xrt_conv_wire_task_desc
	task(uint64_t client, uint32_t t)
	{
		xrt_conv_wire_task_desc d{};
		uint64_t ik = 0, ek = 0;
		REQUIRE(ipc_server_conv_v4_enum_task(sc, client, t, &d, &ik, &ek) == XRT_SUCCESS);
		return d;
	}

	std::vector<xrt_conv_wire_event>
	drain(uint64_t client)
	{
		std::vector<xrt_conv_wire_event> out;
		for (int i = 0; i < 64; i++) {
			bool has = false;
			xrt_conv_wire_event ev{};
			REQUIRE(ipc_server_conv_v4_poll_event(sc, client, &has, &ev) == XRT_SUCCESS);
			if (!has) {
				break;
			}
			out.push_back(ev);
		}
		return out;
	}

	xrt_dp_lift_caps
	properties()
	{
		xrt_dp_lift_caps caps;
		ipc_server_conv_lift_properties(sc, &caps);
		return caps;
	}
};

//! Per task: each event's previous_state is the state of the one before it,
//! and no event repeats an unchanged state (progress events aside).
inline void
check_chain(const std::vector<xrt_conv_wire_event> &evs, uint32_t task, uint32_t baseline)
{
	uint32_t last = baseline;
	for (const auto &e : evs) {
		if (e.kind != XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED || e.task != task) {
			continue;
		}
		CHECK(e.previous_state == last);
		last = e.state;
	}
}

} // namespace
