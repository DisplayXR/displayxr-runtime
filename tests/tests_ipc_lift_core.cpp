// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2005: the XR_DXR_lift v3 handlers routed to the conversion core
 *         (server/ipc_server_conv.h), driven with the CPU fake provider.
 *
 * tests_oxr_lift_startup pins the v3 startup sequence on the LEGACY path from
 * the app's side; this file pins the same sequence once the handlers route to
 * the core, at the server-side adapter the handlers call (the routing rule,
 * properties, create / submit / acquire / export / destroy, the prewarm hint,
 * connection teardown):
 *
 *  - nothing routes without the GPU I/O glue (#2003): shipping v3 behaviour
 *    is unchanged until it lands;
 *  - cold boot: properties ACTIVATING, then READY, without a restart (bug G5
 *    from the app's side) - and never UNAVAILABLE while discovery is open;
 *  - a create while ACTIVATING is accepted and served once READY (G3);
 *  - UNAVAILABLE -> READY after a simulated provider install
 *    (conv_core_provider_changed);
 *  - G2: an NVIEW viewCount above the task's max_views is refused at submit;
 *  - the measured wire limits for roadmap §11.2.
 */

#include "catch_amalgamated.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "conversion/conv_readiness.h"

#include "server/ipc_server_conv.h"
#ifndef _WIN32
#include "server/ipc_server_conv_cpu.h"
#include "shared/ipc_cpu_buffer.h"
#endif
#include "shared/ipc_protocol.h"

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_limits.h"

#include "os/os_time.h"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// roadmap §11.2 (#2005): the conv_* wire is sized against the IPC buffer.
static_assert(XRT_CONV_WIRE_MSG_MAX_BYTES == IPC_BUF_SIZE, "XRT_CONV_WIRE_MSG_MAX_BYTES != IPC_BUF_SIZE");
static_assert(XRT_CONV_WIRE_MAX_SUBMIT_IMAGES * 2 <= XRT_MAX_IPC_HANDLES, "conv_submit handles > XRT_MAX_IPC_HANDLES");
static_assert(XRT_STATUS_CONV_ROW_REPLY_MAX == IPC_BUF_SIZE, "DIAG row reply limit != IPC_BUF_SIZE");
static_assert(sizeof(xrt_conv_wire_hint_interest) == 16, "conv_hint_interest request is 16 bytes");

namespace {

constexpr uint64_t MS = 1000000ull;

void
sleep_ms(uint32_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool
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

xrt_graphics_buffer_handle_t
fake_buffer(intptr_t v)
{
	return (xrt_graphics_buffer_handle_t)v;
}

std::atomic<int> g_live{0};

bool
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
	g_live++;
	return true;
}

void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
	g_live--;
}

//! The #2003 GPU I/O glue, on the CPU: records everything.
struct Glue
{
	std::mutex m;
	int imports = 0, releases = 0, publishes = 0;
	std::vector<uint64_t> gone;
	std::map<uint64_t, uint32_t> export_w; // stream -> export width (0 = none yet)
	uint64_t fence = 0;
	uint32_t last_view_count = 0;
};

xrt_result_t
g_import(void *ud, uint64_t, xrt_graphics_buffer_handle_t, bool, uint32_t, uint32_t, const conv_submit_slot *slot)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	g->imports++;
	if (slot->image_count < 1 || slot->images[0].cpu_ptr == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	std::memset(slot->images[0].cpu_ptr, 0x80, (size_t)slot->images[0].byte_size); // S0
	return XRT_SUCCESS;
}

void
g_release(void *ud, xrt_graphics_buffer_handle_t, bool)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	g->releases++;
}

xrt_result_t
g_publish(void *ud, uint64_t stream_id, const conv_result *r, ipc_server_conv_published *out)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	g->publishes++;
	const xrt_conv_image &src = (r->written & XRT_CONV_OUT_VIEWS) != 0 ? r->views : r->depth;
	uint32_t &w = g->export_w[stream_id];
	out->realloc = w != src.width;
	w = src.width;
	out->width = src.width;
	out->height = src.height;
	out->format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
	out->fence_value = ++g->fence;
	g->last_view_count = r->views_info.view_count;
	return XRT_SUCCESS;
}

bool
g_export(void *ud, uint64_t stream_id, bool depth, xrt_graphics_buffer_handle_t *h, uint32_t *w, uint32_t *hh, uint32_t *f)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	auto it = g->export_w.find(stream_id);
	if (it == g->export_w.end() || it->second == 0) {
		return false;
	}
	*h = fake_buffer(depth ? 0x0DE0 : 0x0BEE0);
	*w = it->second;
	*hh = 1;
	*f = 28;
	return true;
}

bool
g_fence(void *, uint64_t, xrt_graphics_sync_handle_t *h)
{
	*h = (xrt_graphics_sync_handle_t)(intptr_t)0x0FE40;
	return true;
}

void
g_gone(void *ud, uint64_t stream_id)
{
	auto *g = static_cast<Glue *>(ud);
	std::lock_guard<std::mutex> lk(g->m);
	g->gone.push_back(stream_id);
	g->export_w.erase(stream_id);
}

struct Fixture
{
	conv_core *core = nullptr;
	ipc_server_conv *sc = nullptr;
	Glue glue;

	//! The service's wiring of the core's events into the spec-4 queues (service_conversion.c).
	static void
	fwd_event(void *ud, uint64_t client_id, const xrt_conv_wire_event *ev)
	{
		ipc_server_conv_on_event(*static_cast<ipc_server_conv **>(ud), client_id, ev);
	}

	explicit Fixture(uint32_t prewarm_on, bool events = false)
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = prewarm_on;
		if (events) {
			cfg.on_event = fwd_event;
			cfg.event_ud = &sc; // set below, before any provider exists
		}
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		REQUIRE(ipc_server_conv_create(core, &sc) == XRT_SUCCESS);
		ipc_server_set_conv(sc);
	}
	~Fixture()
	{
		ipc_server_set_conv(nullptr);
		ipc_server_conv_destroy(&sc);
		conv_core_destroy(&core);
	}

	void
	install_glue()
	{
		ipc_server_conv_io_ops io{};
		io.import_input = g_import;
		io.release_input = g_release;
		io.publish_result = g_publish;
		io.export_output = g_export;
		io.export_fence = g_fence;
		io.stream_gone = g_gone;
		io.ud = &glue;
		ipc_server_conv_set_io(sc, &io);
	}

	int32_t
	add(conv_fake_provider &f, bool live = true)
	{
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		if (live) {
			d.iface = &f.iface;
			d.inst = conv_fake_inst(&f);
		}
		return conv_core_add_provider(core, &d);
	}

	/*! What ipc_handle_lift_get_properties answers: the core when the rule
	 *  routes, else "modes 0, UNAVAILABLE" (P1b, #2028: no pre-core path). */
	xrt_dp_lift_caps
	properties()
	{
		xrt_dp_lift_caps caps;
		if (ipc_server_conv_routes_properties(ipc_server_get_conv())) {
			ipc_server_conv_lift_properties(ipc_server_get_conv(), &caps);
		} else {
			xrt_dp_lift_caps_init(&caps);
		}
		return caps;
	}

	//! ipc_handle_lift_stream_create: the core when it routes, else FEATURE_UNSUPPORTED.
	xrt_result_t
	create(uint64_t client, uint32_t mode, uint64_t *id, uint32_t aux = 0)
	{
		if (!ipc_server_conv_routes_create(sc, mode, 0)) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // not routable: refused on the v3 wire
		}
		return ipc_server_conv_lift_stream_create(sc, client, mode, 0, 1.0f, aux, 0, id);
	}

	xrt_result_t
	submit(uint64_t client, uint64_t id, uint32_t view_count = 0, bool params = false)
	{
		ipc_arg_lift_submit a{};
		a.stream_id = id;
		a.source_time = 42;
		a.width = 64;
		a.height = 32;
		a.has_params = params ? 1u : 0u;
		a.params.convergence = -1.0f;
		a.params.strength = 1.0f;
		a.params.view_count = view_count;
		uint64_t fid = 0;
		return ipc_server_conv_lift_submit(sc, client, &a, fake_buffer(0x1234), false, &fid);
	}
};

} // namespace


TEST_CASE("no GPU glue: nothing routes, the shipping v3 path is untouched", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		REQUIRE(fx.add(f) >= 0);
		CHECK_FALSE(ipc_server_conv_routes_properties(fx.sc));
		CHECK_FALSE(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_SBS, 0));
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, 1));
		// An incomplete glue is refused: still nothing routes.
		ipc_server_conv_io_ops half{};
		half.import_input = g_import;
		ipc_server_conv_set_io(fx.sc, &half);
		CHECK_FALSE(ipc_server_conv_routes_properties(fx.sc));
		// Nor without a registered object (the handlers' NULL case).
		ipc_server_set_conv(nullptr);
		CHECK_FALSE(ipc_server_conv_routes_properties(ipc_server_get_conv()));
		CHECK_FALSE(ipc_server_conv_owns_stream(ipc_server_get_conv(), IPC_SERVER_CONV_STREAM_TAG | 1));
		ipc_server_set_conv(fx.sc);
		// GAUSSIANS without a SPLATS_MONO provider never routes, glue or not: an
		// honest FEATURE_UNSUPPORTED (P1b deleted the pre-core fall-through).
		fx.install_glue();
		CHECK_FALSE(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_GAUSSIANS, 1));
		uint64_t gid = 0;
		CHECK(fx.create(9, XRT_DP_LIFT_MODE_GAUSSIANS, &gid) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(gid == 0u);
		CHECK(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_SBS, 0));
	}
	conv_fake_fini(&f);
}

TEST_CASE("cold boot: ACTIVATING before the first describe, then READY without a restart (G5)", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 0);
	v->warm_ms = 60;
	v->cap.max_streams = 3;
	v->cap.est_latency_ms = 25;
	conv_fake_add_cap(&f, 1, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0)->warm_ms = 60;
	{
		Fixture fx(CONV_PREWARM_INTEREST);
		fx.install_glue();

		// Registered, not yet described (the loader found it; probe/describe pending).
		REQUIRE(fx.add(f, /*live=*/false) >= 0);
		REQUIRE_FALSE(conv_core_discovery_settled(fx.core));
		xrt_dp_lift_caps p = fx.properties();
		CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING); // properties before the first describe
		CHECK(p.modes == 0u);                            // ACTIVATING even before any mode bit

		// Described: rows INSTALLED (cold). Still ACTIVATING, modes advertised.
		REQUIRE(fx.add(f) >= 0);
		CHECK(conv_core_discovery_settled(fx.core));
		p = fx.properties();
		CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING);
		CHECK(p.modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW));
		CHECK(p.max_views == 8u);
		CHECK(p.max_streams == 3u);
		CHECK(p.typical_latency_ns == 25u * MS);
		CHECK(p.aux_outputs == XRT_DP_LIFT_AUX_DEPTH); // G1's field, set from the core
		CHECK(p.aux_depth_semantics == XRT_DP_LIFT_DEPTH_METRIC);
		CHECK(std::strcmp(p.backend, "conversion") == 0); // no provider identity

		// The session's conv_hint_interest (PrewarmOn = interest) warms the
		// default tasks: the app's next polls see READY, same service.
		ipc_server_conv_hint_interest(fx.sc, 7, 0);
		REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 3000));
		CHECK(fx.properties().modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW));
	}
	conv_fake_fini(&f);
}

TEST_CASE("first-create policy: the hint does not warm; a create does", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0)->warm_ms = 30;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		ipc_server_conv_hint_interest(fx.sc, 7, 0);
		sleep_ms(150);
		CHECK(conv_core_task_state(fx.core, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_INSTALLED);
		uint64_t id = 0;
		REQUIRE(fx.create(7, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
		REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 3000));
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 7, id) == XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("create while ACTIVATING is accepted and served once READY (G3)", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 0);
	v->warm_ms = 300;
	v->convert_ms = 2;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		REQUIRE(fx.properties().state == XRT_DP_LIFT_STATE_ACTIVATING);

		uint64_t id = 0;
		REQUIRE(fx.create(9, XRT_DP_LIFT_MODE_SBS, &id, XRT_DP_LIFT_AUX_DEPTH) == XRT_SUCCESS);
		CHECK((id & IPC_SERVER_CONV_STREAM_TAG) != 0u);
		CHECK(ipc_server_conv_owns_stream(fx.sc, id));
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, 1)); // an id this route never minted

		// Frames submitted while warming wait (latest-wins); NOT READY meanwhile.
		REQUIRE(fx.submit(9, id) == XRT_SUCCESS);
		bool ready = true;
		ipc_lift_result r{};
		REQUIRE(ipc_server_conv_lift_acquire(fx.sc, 9, id, &ready, &r) == XRT_SUCCESS);
		CHECK_FALSE(ready);

		// Another connection does not own it.
		CHECK(ipc_server_conv_lift_acquire(fx.sc, 10, id, &ready, &r) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);

		REQUIRE(wait_until(
		    [&] {
			    fx.submit(9, id);
			    return ipc_server_conv_lift_acquire(fx.sc, 9, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    4000));
		CHECK(fx.properties().state == XRT_DP_LIFT_STATE_READY);
		CHECK(r.frame_id >= 1u);
		CHECK(r.output_realloc == 1u); // first result: the client opens the export texture
		CHECK(r.view_count == 2u);
		CHECK(r.width == 128u); // 2 views x 64, ROW layout
		CHECK(r.fence_value >= 1u);
		CHECK(r.depth.valid == 1u); // aux depth (advisory, declared by the provider)
		CHECK(r.depth.units == XRT_DP_LIFT_DEPTH_METRIC);

		// The v3 export calls, then a steady-state acquire (no realloc).
		xrt_graphics_buffer_handle_t h{};
		uint32_t w = 0, hh = 0, fmt = 0;
		CHECK(ipc_server_conv_lift_export_output(fx.sc, 9, id, false, &h, &w, &hh, &fmt));
		CHECK(w == 128u);
		xrt_graphics_sync_handle_t fh{};
		CHECK(ipc_server_conv_lift_export_fence(fx.sc, 9, id, &fh));
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(9, id);
			    return ipc_server_conv_lift_acquire(fx.sc, 9, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    2000));
		CHECK(r.output_realloc == 0u);

		xrt_lift_stream_stats st{};
		REQUIRE(ipc_server_conv_lift_stats(fx.sc, 9, id, &st) == XRT_SUCCESS);
		CHECK(st.submitted >= 2u);
		CHECK(st.converted >= 1u);
		CHECK(ipc_server_conv_lift_set_priority(fx.sc, 9, id, 1) == XRT_SUCCESS);
		REQUIRE(ipc_server_conv_lift_stats(fx.sc, 9, id, &st) == XRT_SUCCESS);
		CHECK(st.priority == 1u);

		// Every submit's input handle was consumed (imported).
		{
			std::lock_guard<std::mutex> lk(fx.glue.m);
			CHECK(fx.glue.releases == 0);
			CHECK(fx.glue.imports >= 3);
		}

		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 9, id) == XRT_SUCCESS);
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, id));
		std::lock_guard<std::mutex> lk(fx.glue.m);
		REQUIRE(fx.glue.gone.size() == 1u);
		CHECK(fx.glue.gone[0] == id);
	}
	conv_fake_fini(&f);
}

TEST_CASE("UNAVAILABLE -> READY after a provider install, without a restart", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->state = XRT_CONV_STATE_ABSENT; // its model is not installed yet
	v->warm_ms = 30;
	{
		Fixture fx(CONV_PREWARM_INTEREST);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		REQUIRE(conv_core_discovery_settled(fx.core));
		CHECK(fx.properties().state == XRT_DP_LIFT_STATE_UNAVAILABLE);
		CHECK(fx.properties().modes == 0u);
		uint64_t id = 0;
		CHECK(fx.create(3, XRT_DP_LIFT_MODE_SBS, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		ipc_server_conv_hint_interest(fx.sc, 3, 0);

		// The installer lands the model; the registry watch says provider_changed.
		conv_fake_set_state(&f, 4, XRT_CONV_STATE_INSTALLED, 0, true);
		conv_core_provider_changed(fx.core, "fake");
		REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 3000));
		CHECK((fx.properties().modes & XRT_DP_LIFT_MODE_SBS) != 0u);
		REQUIRE(fx.create(3, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 3, id) == XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("conv_warm (DIAG): warms the selection; READY is a no-op; a terminal task is retried (#2029)",
          "[ipc_lift_core]")
{
	uint32_t prev = 99, now = 99;
	// No conversion core in the service (DXR_CONV=0): refused, states zeroed.
	CHECK(ipc_server_conv_warm(nullptr, XRT_CONV_TASK_VIEWS_MONO, &prev, &now) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(prev == 0u);
	CHECK(now == 0u);

	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->warm_ms = 30;
	conv_fake_cap *d =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	d->warm_ms = 30;
	d->warm_end_state = XRT_CONV_STATE_ABSENT; // the provider tries, then says "not here"
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		REQUIRE(conv_core_task_state(fx.core, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_INSTALLED);

		// Cold: warmed, never blocking.
		REQUIRE(ipc_server_conv_warm(fx.sc, XRT_CONV_TASK_VIEWS_MONO, &prev, &now) == XRT_SUCCESS);
		CHECK(prev == XRT_CONV_STATE_INSTALLED);
		CHECK(now == XRT_CONV_STATE_WARMING);
		REQUIRE(wait_until(
		    [&] { return conv_core_task_state(fx.core, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_READY; },
		    3000));
		uint64_t warms = 0;
		{
			os_mutex_lock(&f.mtx);
			warms = f.warms;
			os_mutex_unlock(&f.mtx);
		}
		// READY: a no-op that says so; the provider is not called again.
		REQUIRE(ipc_server_conv_warm(fx.sc, XRT_CONV_TASK_VIEWS_MONO, &prev, &now) == XRT_SUCCESS);
		CHECK(prev == XRT_CONV_STATE_READY);
		CHECK(now == XRT_CONV_STATE_READY);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		os_mutex_lock(&f.mtx);
		CHECK(f.warms == warms);
		os_mutex_unlock(&f.mtx);

		// A task nothing serves: refused, ABSENT (discovery settled).
		CHECK(ipc_server_conv_warm(fx.sc, XRT_CONV_TASK_DEPTH_MONO, &prev, &now) ==
		      XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(prev == XRT_CONV_STATE_ABSENT);

		// A provider that declares itself absent after the warm: terminal.
		REQUIRE(ipc_server_conv_warm(fx.sc, XRT_CONV_TASK_DEPTH_VIDEO, &prev, &now) == XRT_SUCCESS);
		CHECK(prev == XRT_CONV_STATE_INSTALLED);
		REQUIRE(wait_until(
		    [&] { return conv_core_task_state(fx.core, XRT_CONV_TASK_DEPTH_VIDEO) == XRT_CONV_STATE_ABSENT; },
		    3000));
		// The module got installed: an explicit warm is the retry trigger.
		os_mutex_lock(&f.mtx);
		d->warm_end_state = 0;
		os_mutex_unlock(&f.mtx);
		REQUIRE(ipc_server_conv_warm(fx.sc, XRT_CONV_TASK_DEPTH_VIDEO, &prev, &now) == XRT_SUCCESS);
		CHECK(prev == XRT_CONV_STATE_ABSENT);
		CHECK(now == XRT_CONV_STATE_WARMING);
		REQUIRE(wait_until(
		    [&] { return conv_core_task_state(fx.core, XRT_CONV_TASK_DEPTH_VIDEO) == XRT_CONV_STATE_READY; },
		    3000));
	}
	conv_fake_fini(&f);
}

TEST_CASE("4-view stream created while ACTIVATING converts once READY on a module that has NVIEW (#2029)",
          "[ipc_lift_core]")
{
	// The dp-lift shape: the describe says 2 views (the conservative set)
	// until the module is READY, then the refined describe says 6. A 4-view
	// stream created and fed meanwhile is accepted, waits, and converts.
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->cap.max_views = 2;
	v->warm_ms = 150;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		CHECK((fx.properties().modes & XRT_DP_LIFT_MODE_NVIEW) == 0u); // not advertised while provisional
		uint64_t id = 0;
		REQUIRE(fx.create(5, XRT_DP_LIFT_MODE_NVIEW, &id) == XRT_SUCCESS);    // ...but accepted
		CHECK(fx.submit(5, id, 4, true) == XRT_SUCCESS);                      // 4 views > the provisional 2
		CHECK(fx.submit(5, id, 9, true) == XRT_ERROR_OUTPUT_REQUEST_FAILURE); // the wire's own cap still holds

		// READY on the conservative describe first: the stream waits (unsatisfied,
		// not latched) until the refined describe brings the views.
		REQUIRE(wait_until(
		    [&] { return conv_core_task_state(fx.core, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_READY; },
		    3000));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		os_mutex_lock(&f.mtx);
		v->cap.max_views = 6; // the module has NVIEW: the refined describe
		f.generation++;
		os_mutex_unlock(&f.mtx);

		bool ready = false;
		ipc_lift_result r{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(5, id, 4, true);
			    return ipc_server_conv_lift_acquire(fx.sc, 5, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    3000));
		CHECK(r.view_count == 4u);
		CHECK(r.width == 4u * 64u);
	}
	conv_fake_fini(&f);

	// A module WITHOUT N-view: the same stream goes unsatisfied at READY (the
	// #2027 path) -- it never converts, nothing crashes, it is not refused late.
	conv_fake_provider g;
	conv_fake_init(&g, "fake2");
	conv_fake_cap *w =
	    conv_fake_add_cap(&g, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	w->cap.max_views = 2;
	w->warm_ms = 50;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(g) >= 0);
		uint64_t id = 0;
		REQUIRE(fx.create(6, XRT_DP_LIFT_MODE_NVIEW, &id) == XRT_SUCCESS);
		REQUIRE(fx.submit(6, id, 4, true) == XRT_SUCCESS);
		uint64_t core_id = 0;
		uint32_t mode = 0;
		REQUIRE(ipc_server_conv_resolve_stream(fx.sc, 6, id, &core_id, &mode));
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(6, id, 4, true);
			    conv_stream_stats s{};
			    return conv_core_stream_get_stats(fx.core, core_id, &s) == XRT_SUCCESS && s.unsatisfied;
		    },
		    3000));
		bool ready = false;
		ipc_lift_result r{};
		CHECK(ipc_server_conv_lift_acquire(fx.sc, 6, id, &ready, &r) == XRT_SUCCESS);
		CHECK_FALSE(ready);
	}
	conv_fake_fini(&g);
}

TEST_CASE("G2: NVIEW viewCount is bounded by the task's max_views at submit", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->cap.max_views = 6;
	conv_fake_set_state(&f, 4, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		xrt_dp_lift_caps p = fx.properties();
		CHECK(p.max_views == 6u);
		CHECK((p.modes & XRT_DP_LIFT_MODE_NVIEW) != 0u);

		uint64_t id = 0;
		REQUIRE(fx.create(5, XRT_DP_LIFT_MODE_NVIEW, &id) == XRT_SUCCESS);
		CHECK(fx.submit(5, id, 7, true) == XRT_ERROR_OUTPUT_REQUEST_FAILURE); // > max_views: refused
		CHECK(fx.submit(5, id, 1, true) == XRT_ERROR_OUTPUT_REQUEST_FAILURE); // < 2: refused
		{
			std::lock_guard<std::mutex> lk(fx.glue.m);
			CHECK(fx.glue.releases == 2); // refused inputs are released, never leaked
		}
		// 6 views: the same v3 handle, a resized stream behind it.
		bool ready = false;
		ipc_lift_result r{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(5, id, 6, true);
			    return ipc_server_conv_lift_acquire(fx.sc, 5, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    3000));
		CHECK(r.view_count == 6u);
		CHECK(r.width == 6u * 64u);
	}
	conv_fake_fini(&f);
}

TEST_CASE("connection teardown: conv_core_client_gone drops only that connection's streams", "[ipc_lift_core]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		uint64_t a = 0, b = 0, c = 0;
		REQUIRE(fx.create(1, XRT_DP_LIFT_MODE_SBS, &a) == XRT_SUCCESS);
		REQUIRE(fx.create(1, XRT_DP_LIFT_MODE_SBS, &b) == XRT_SUCCESS);
		REQUIRE(fx.create(2, XRT_DP_LIFT_MODE_SBS, &c) == XRT_SUCCESS);
		ipc_server_conv_client_gone(fx.sc, 1);
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, a));
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, b));
		CHECK(ipc_server_conv_owns_stream(fx.sc, c));
		std::lock_guard<std::mutex> lk(fx.glue.m);
		CHECK(fx.glue.gone.size() == 2u);
	}
	CHECK(g_live.load() == 0);
	conv_fake_fini(&f);
}

namespace {

//! Every allocation request the core made (composed-binding import kinds).
struct ReqLog
{
	std::mutex m;
	std::vector<conv_image_request> reqs;
} g_reqs;

bool
rec_alloc(void *ud, const conv_image_request *req, xrt_conv_image *out)
{
	{
		std::lock_guard<std::mutex> lk(g_reqs.m);
		g_reqs.reqs.push_back(*req);
	}
	return cpu_alloc(ud, req, out);
}

//! A CPU view synthesis: the composed pipeline exists only with one.
bool
cpu_render(void *, const conv_viewsyn_request *r)
{
	const xrt_conv_image *t = r->target;
	if (t->memory_kind != XRT_CONV_MEM_CPU || t->cpu_ptr == nullptr) {
		return false;
	}
	std::memset(t->cpu_ptr, 0x40, (size_t)t->byte_size);
	return true;
}

} // namespace

TEST_CASE("composed binding: the depth stage's import kinds reach the allocator; the views target carries none",
          "[ipc_lift_core]")
{
	// A depth-only provider that imports GPU kinds besides CPU (the shape of
	// an in-service Vulkan provider): an SBS stream over it is COMPOSED (its
	// depth + the runtime's S5). The input it consumes and the depth it writes
	// must be requested with its import kinds -- they used to be 0 because
	// bound_import_kinds skipped composed bindings, which the D3D11 glue (it
	// only checked for NT + keyed mutex) hid -- and the views target the
	// runtime renders must carry none.
	const uint32_t kinds = XRT_CONV_MEM_CPU | XRT_CONV_MEM_VK_OPAQUE_FD | XRT_CONV_MEM_IOSURFACE;
	const uint32_t syncs = (1u << XRT_CONV_SYNC_NONE) | (1u << XRT_CONV_SYNC_VK_SEMAPHORE_FD);
	conv_fake_provider f;
	conv_fake_init(&f, "depth-vk");
	conv_fake_cap *c =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	c->state = XRT_CONV_STATE_READY;
	c->cap.import_memory_kinds = kinds;
	c->cap.import_sync_kinds = syncs;
	{
		std::lock_guard<std::mutex> lk(g_reqs.m);
		g_reqs.reqs.clear();
	}
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = rec_alloc;
		cfg.alloc.free = cpu_free;
		cfg.viewsyn.render = cpu_render;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		conv_core *core = nullptr;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		ipc_server_conv *sc = nullptr;
		REQUIRE(ipc_server_conv_create(core, &sc) == XRT_SUCCESS);
		ipc_server_set_conv(sc);
		Glue glue;
		{
			ipc_server_conv_io_ops io{};
			io.import_input = g_import;
			io.release_input = g_release;
			io.publish_result = g_publish;
			io.export_output = g_export;
			io.export_fence = g_fence;
			io.stream_gone = g_gone;
			io.ud = &glue;
			ipc_server_conv_set_io(sc, &io);
		}
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(core, &d) >= 0);

		REQUIRE(ipc_server_conv_routes_create(sc, XRT_DP_LIFT_MODE_SBS, 0));
		uint64_t id = 0;
		REQUIRE(ipc_server_conv_lift_stream_create(sc, 7, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &id) ==
		        XRT_SUCCESS);
		uint64_t core_id = 0;
		uint32_t mode = 0;
		REQUIRE(ipc_server_conv_resolve_stream(sc, 7, id, &core_id, &mode));
		bool ready = false;
		ipc_lift_result r{};
		REQUIRE(wait_until(
		    [&] {
			    ipc_arg_lift_submit a{};
			    a.stream_id = id;
			    a.source_time = 42;
			    a.width = 64;
			    a.height = 32;
			    uint64_t fid = 0;
			    (void)ipc_server_conv_lift_submit(sc, 7, &a, fake_buffer(0x1234), false, &fid);
			    return ipc_server_conv_lift_acquire(sc, 7, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    3000));
		conv_stream_stats st{};
		REQUIRE(conv_core_stream_get_stats(core, core_id, &st) == XRT_SUCCESS);
		CHECK(st.composed);
		// ADR-058's internal age split: placed at conversion today, so the
		// placement is never older than the content (eyes are sampled after
		// the submit) -- late placement will make it the younger of the two.
		CHECK(st.content_age_ns > 0);
		CHECK(st.placement_age_ns <= st.content_age_ns);

		int inputs = 0, depths = 0, views = 0;
		{
			std::lock_guard<std::mutex> lk(g_reqs.m);
			for (const conv_image_request &q : g_reqs.reqs) {
				if ((q.usage & CONV_IMAGE_USAGE_INPUT) != 0) {
					inputs++;
					CHECK(q.import_memory_kinds == kinds);
					CHECK(q.import_sync_kinds == syncs);
				} else if ((q.usage & CONV_IMAGE_USAGE_RENDER_TARGET) != 0) {
					views++;
					CHECK(q.import_memory_kinds == 0u); // the runtime's S5 renders it
					CHECK(q.import_sync_kinds == 0u);
				} else if ((q.usage & CONV_IMAGE_USAGE_PROVIDER_OUT) != 0 && q.role == XRT_CONV_OUT_DEPTH) {
					depths++;
					CHECK(q.import_memory_kinds == kinds);
					CHECK(q.import_sync_kinds == syncs);
				}
			}
		}
		CHECK(inputs >= 1);
		CHECK(depths >= 1);
		CHECK(views >= 1);

		ipc_server_conv_set_io(sc, nullptr);
		(void)conv_core_stream_destroy(core, core_id);
		ipc_server_set_conv(nullptr);
		ipc_server_conv_destroy(&sc);
		conv_core_destroy(&core);
	}
	CHECK(g_live.load() == 0);
	conv_fake_fini(&f);
}

TEST_CASE("windowless submit rect: the submitted extent's aspect inside the panel; a window rect unchanged",
          "[ipc_lift_core]")
{
	xrt_lift_rect_frame out{};
	// A window / weave rect passes through byte-identical.
	xrt_lift_rect_frame win{};
	win.center[0] = 0.05f;
	win.center[1] = -0.02f;
	win.width_m = 0.20f;
	win.height_m = 0.10f;
	win.valid = true;
	win.eyes_dp_key = 77;
	REQUIRE(ipc_server_conv_submit_rect(&win, 1280, 720, &out));
	CHECK(std::memcmp(&out, &win, sizeof(win)) == 0);

	// Windowless: the panel's size (invalid) + a 16:9 extent on a 5:3 panel:
	// full panel width, the extent's aspect, centred, valid.
	xrt_lift_rect_frame panel{};
	panel.width_m = 0.50f;
	panel.height_m = 0.30f;
	panel.eyes_dp_key = 9;
	REQUIRE(ipc_server_conv_submit_rect(&panel, 1280, 720, &out));
	CHECK(out.valid);
	CHECK(out.width_m == 0.50f);
	CHECK(std::fabs(out.height_m - 0.50f * 720.0f / 1280.0f) < 1e-6f);
	CHECK(out.center[0] == 0.0f);
	CHECK(out.center[1] == 0.0f);
	CHECK(out.eyes_dp_key == 9u);
	// A taller-than-panel extent: full panel height.
	REQUIRE(ipc_server_conv_submit_rect(&panel, 640, 640, &out));
	CHECK(out.height_m == 0.30f);
	CHECK(std::fabs(out.width_m - 0.30f) < 1e-6f);
	// No panel size, no extent, no frame: the rect stays unknown (as before).
	xrt_lift_rect_frame none{};
	CHECK_FALSE(ipc_server_conv_submit_rect(&none, 1280, 720, &out));
	CHECK_FALSE(ipc_server_conv_submit_rect(&panel, 0, 720, &out));
	CHECK_FALSE(ipc_server_conv_submit_rect(nullptr, 1280, 720, &out));
}

#ifndef _WIN32
TEST_CASE("spec-4 link stream (no window): converts at the submitted extent, against an extent-aspect rect",
          "[ipc_lift_core]")
{
	// A windowless connection submits 320x180 on a 0.6 x 0.4 m panel: the
	// provider sees a rect of the EXTENT's aspect (16:9, panel-wide), and the
	// result is 2 x 320 wide -- the panel never sets the resolution. A
	// windowed submit's rect reaches the provider unchanged.
	ipc_server_conv_io_ops cpu_io;
	if (!ipc_server_conv_cpu_io_get(&cpu_io)) {
		SKIP("no CPU buffer transport on this platform");
	}
	conv_fake_provider f;
	conv_fake_init(&f, "views");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->state = XRT_CONV_STATE_READY;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		ipc_server_conv_set_io(fx.sc, &cpu_io);
		REQUIRE(fx.add(f) >= 0);
		xrt_conv_wire_stream_desc sd{};
		sd.struct_size = sizeof(sd);
		sd.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
		sd.task = XRT_CONV_TASK_VIEWS_MONO;
		sd.input_kind = XRT_CONV_INPUT_MONO;
		sd.view_count = 2;
		sd.content_hint = XRT_CONV_CONTENT_VIDEO;
		xrt_conv_wire_stream_created cr{};
		REQUIRE(ipc_server_conv_v4_stream_create(fx.sc, 1, &sd, 0, 0, 1.0f, &cr) == XRT_SUCCESS);

		const uint32_t W = 320, H = 180;
		auto convert = [&](const xrt_lift_rect_frame &frame, ipc_lift_result &r3) {
			bool ready = false;
			return wait_until(
			    [&] {
				    std::vector<uint8_t> px((size_t)W * H * 4, 0x50);
				    xrt_graphics_buffer_handle_t h = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
				    REQUIRE(ipc_cpu_buffer_create(px.size(), &h));
				    conv_input_import in{};
				    in.kind = CONV_INPUT_CPU;
				    in.handle = h;
				    in.sync_handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
				    in.cpu_row_pitch = W * 4;
				    ipc_arg_lift_submit a{};
				    a.stream_id = cr.stream_id;
				    a.source_time = 42;
				    a.width = W;
				    a.height = H;
				    uint64_t fid = 0;
				    (void)ipc_server_conv_v4_submit(fx.sc, 1, &a, &in, &frame, &fid);
				    bool disc = false;
				    xrt_conv_wire_result r4{};
				    return ipc_server_conv_v4_acquire(fx.sc, 1, cr.stream_id, &ready, &disc, &r3, &r4) ==
				               XRT_SUCCESS &&
				           ready;
			    },
			    3000);
		};

		xrt_lift_rect_frame windowless{};
		windowless.width_m = 0.60f; // the panel, invalid: what the handler fills
		windowless.height_m = 0.40f;
		ipc_lift_result r3{};
		REQUIRE(convert(windowless, r3));
		CHECK(r3.width == 2u * W); // output extent == input extent, per view
		CHECK(r3.height == H);
		os_mutex_lock(&f.mtx);
		const float rw = f.last_params.rect_width_m, rh = f.last_params.rect_height_m;
		os_mutex_unlock(&f.mtx);
		CHECK(rw == 0.60f);
		CHECK(std::fabs(rh - 0.60f * (float)H / (float)W) < 1e-5f);

		xrt_lift_rect_frame win{};
		win.width_m = 0.20f;
		win.height_m = 0.12f;
		win.valid = true;
		REQUIRE(convert(win, r3));
		os_mutex_lock(&f.mtx);
		CHECK(f.last_params.rect_width_m == 0.20f);
		CHECK(f.last_params.rect_height_m == 0.12f);
		os_mutex_unlock(&f.mtx);

		ipc_server_conv_set_io(fx.sc, nullptr);
	}
	conv_fake_fini(&f);
}
#endif

TEST_CASE("roadmap 11.2: IPC handle capacity and reply sizes (measured)", "[ipc_lift_core]")
{
	// One line per measured limit; the static_asserts above are the gate.
	std::printf("[11.2] IPC_BUF_SIZE=%d XRT_MAX_IPC_HANDLES=%d\n", (int)IPC_BUF_SIZE, (int)XRT_MAX_IPC_HANDLES);
	std::printf("[11.2] v3 lift_submit_frame in-handles=1, lift_get_output/_depth_output/_fence out-handles=1\n");
	std::printf("[11.2] conv_submit worst case handles=%d (images %u x (image + sync))\n",
	            (int)(XRT_CONV_WIRE_MAX_SUBMIT_IMAGES * 2), XRT_CONV_WIRE_MAX_SUBMIT_IMAGES);
	std::printf("[11.2] sizeof: conv_submit=%zu conv_result=%zu task_desc=%zu job_desc=%zu job_result=%zu "
	            "hint_interest=%zu ipc_arg_lift_submit=%zu ipc_lift_result=%zu\n",
	            sizeof(xrt_conv_wire_submit), sizeof(xrt_conv_wire_result), sizeof(xrt_conv_wire_task_desc),
	            sizeof(xrt_conv_wire_job_desc), sizeof(xrt_conv_wire_job_result),
	            sizeof(xrt_conv_wire_hint_interest), sizeof(ipc_arg_lift_submit), sizeof(ipc_lift_result));
	std::printf("[11.2] DIAG rows: provider=%zu task=%zu generation=%zu limit=%d\n",
	            sizeof(xrt_status_conv_provider), sizeof(xrt_status_conv_task), sizeof(xrt_status_generation),
	            (int)XRT_STATUS_CONV_ROW_REPLY_MAX);
	CHECK(sizeof(xrt_conv_wire_submit) <= IPC_BUF_SIZE);
	CHECK(sizeof(xrt_conv_wire_result) <= IPC_BUF_SIZE);
	CHECK(sizeof(xrt_status_conv_provider) + sizeof(xrt_status_generation) + 16 <= IPC_BUF_SIZE);
	CHECK(sizeof(xrt_status_conv_task) + sizeof(xrt_status_generation) + 16 <= IPC_BUF_SIZE);
}


/*
 *
 * Lift spec 5 (#2178): read-only candidates + the per-create preference hint,
 * at the server-side adapter conv_enum_candidates / conv_stream_create_v5 call.
 *
 */

namespace {

constexpr uint32_t TASK = XRT_CONV_TASK_DEPTH_VIDEO;

int32_t
add_ordered(Fixture &fx, conv_fake_provider &f, uint32_t probe_order)
{
	conv_provider_desc d{};
	d.id = f.id;
	d.probe_order = probe_order;
	d.enabled = true;
	d.iface = &f.iface;
	d.inst = conv_fake_inst(&f);
	return conv_core_add_provider(fx.core, &d);
}

conv_fake_cap *
depth_cap(conv_fake_provider &f, const char *display_name, uint32_t state)
{
	conv_fake_cap *c = conv_fake_add_cap(&f, 1, TASK, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	std::snprintf(c->cap.display_name, sizeof(c->cap.display_name), "%s", display_name);
	c->state = state;
	return c;
}

//! A spec-5 create for TASK on connection @p client with @p hint (nullptr = none).
xrt_result_t
create_v5(Fixture &fx, uint64_t client, const char *hint, uint64_t *out_id)
{
	xrt_conv_wire_stream_desc sd;
	xrt_conv_wire_begin(&sd, (uint32_t)sizeof(sd));
	sd.task = TASK;
	sd.input_kind = XRT_CONV_INPUT_MONO;
	xrt_conv_wire_stream_created c{};
	const xrt_result_t r = ipc_server_conv_v5_stream_create(fx.sc, client, &sd, 0, 0, 1.0f, hint, &c);
	*out_id = c.stream_id;
	return r;
}

uint32_t
streams_on(Fixture &fx, const char *provider)
{
	uint32_t streams = 0, jobs = 0;
	conv_core_row_activity(fx.core, provider, 1, false, &streams, &jobs);
	return streams;
}

std::vector<xrt_conv_wire_candidate>
candidates(Fixture &fx, uint64_t client)
{
	std::vector<xrt_conv_wire_candidate> out;
	uint32_t count = 0;
	xrt_conv_wire_generation gen{};
	xrt_conv_wire_candidate c{};
	REQUIRE(ipc_server_conv_v5_enum_candidates(fx.sc, client, TASK, 0, &count, &gen, &c) == XRT_SUCCESS);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t n = 0;
		REQUIRE(ipc_server_conv_v5_enum_candidates(fx.sc, client, TASK, i, &n, &gen, &c) == XRT_SUCCESS);
		REQUIRE(n == count);
		out.push_back(c);
	}
	return out;
}

bool
flag(const xrt_conv_wire_candidate &c, uint32_t f)
{
	return (c.candidate_flags & f) != 0;
}

void
set_preferred(Fixture &fx, const char *id)
{
	const char *pref[XRT_CONV_TASK_RESERVED_FIRST] = {};
	pref[TASK] = id;
	(void)conv_core_set_policy(fx.core, pref, false);
}

} // namespace

TEST_CASE("spec 5: enumeration lists every candidate, marks isSelected / isUserDefault", "[ipc_lift_core][lift_spec5]")
{
	conv_fake_provider a, b;
	conv_fake_init(&a, "alpha");
	conv_fake_init(&b, "beta");
	depth_cap(a, "Alpha Depth (provider's own name)", XRT_CONV_STATE_READY);
	depth_cap(b, "Beta Depth", XRT_CONV_STATE_READY);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(add_ordered(fx, a, 50) >= 0);
		REQUIRE(add_ordered(fx, b, 100) >= 0);

		auto c = candidates(fx, 5);
		REQUIRE(c.size() == 2u);
		CHECK(std::strcmp(c[0].provider_id, "alpha") == 0); // selection order: ProbeOrder
		CHECK(std::strcmp(c[1].provider_id, "beta") == 0);
		CHECK(std::strcmp(c[0].display_name, "Alpha Depth (provider's own name)") == 0); // verbatim
		CHECK(c[0].task == TASK);
		CHECK(c[0].state == XRT_CONV_STATE_READY);
		CHECK(c[0].licence_class == XRT_CONV_LICENCE_PERMISSIVE);
		CHECK(c[0].hosting == XRT_CONV_WIRE_HOSTING_LOCAL_IN_SERVICE);
		CHECK(flag(c[0], XRT_CONV_WIRE_CANDIDATE_SELECTED));
		CHECK_FALSE(flag(c[1], XRT_CONV_WIRE_CANDIDATE_SELECTED));
		CHECK_FALSE(flag(c[0], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT)); // no PreferredProvider set
		CHECK_FALSE(flag(c[1], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT));

		// The user's default (the CLI / dashboard verb) moves both flags.
		set_preferred(fx, "beta");
		c = candidates(fx, 5);
		REQUIRE(c.size() == 2u);
		CHECK(std::strcmp(c[0].provider_id, "beta") == 0);
		CHECK(flag(c[0], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT));
		CHECK(flag(c[0], XRT_CONV_WIRE_CANDIDATE_SELECTED));
		CHECK_FALSE(flag(c[1], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT));
		CHECK_FALSE(flag(c[1], XRT_CONV_WIRE_CANDIDATE_SELECTED));

		// An index past the count is the count + an empty candidate, not an error.
		uint32_t n = 0;
		xrt_conv_wire_generation g{};
		xrt_conv_wire_candidate e{};
		CHECK(ipc_server_conv_v5_enum_candidates(fx.sc, 5, TASK, 7, &n, &g, &e) == XRT_SUCCESS);
		CHECK(n == 2u);
		CHECK(e.provider_id[0] == '\0');
		// A reserved task is a validation failure.
		CHECK(ipc_server_conv_v5_enum_candidates(fx.sc, 5, XRT_CONV_TASK_INPAINT, 0, &n, &g, &e) ==
		      XRT_ERROR_INPUT_UNSUPPORTED);
	}
	conv_fake_fini(&a);
	conv_fake_fini(&b);
}

TEST_CASE("spec 5: enumeration with zero providers returns 0, not an error", "[ipc_lift_core][lift_spec5]")
{
	uint32_t n = 99;
	xrt_conv_wire_generation g{};
	xrt_conv_wire_candidate c{};
	// No conversion service at all.
	CHECK(ipc_server_conv_v5_enum_candidates(nullptr, 1, TASK, 0, &n, &g, &c) == XRT_SUCCESS);
	CHECK(n == 0u);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		n = 99;
		CHECK(ipc_server_conv_v5_enum_candidates(fx.sc, 1, TASK, 0, &n, &g, &c) == XRT_SUCCESS);
		CHECK(n == 0u);
		CHECK(conv_core_enum_candidates(fx.core, TASK, nullptr, 0) == 0u);
	}
}

TEST_CASE("spec 5: a hint to a READY non-default provider selects it", "[ipc_lift_core][lift_spec5]")
{
	conv_fake_provider a, b;
	conv_fake_init(&a, "alpha");
	conv_fake_init(&b, "beta");
	depth_cap(a, "Alpha", XRT_CONV_STATE_READY);
	depth_cap(b, "Beta", XRT_CONV_STATE_READY);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(add_ordered(fx, a, 50) >= 0);
		REQUIRE(add_ordered(fx, b, 100) >= 0);
		REQUIRE(conv_core_task_state(fx.core, TASK) == XRT_CONV_STATE_READY);

		uint64_t plain = 0, hinted = 0;
		REQUIRE(create_v5(fx, 3, nullptr, &plain) == XRT_SUCCESS);
		CHECK(streams_on(fx, "alpha") == 1u); // no hint: the default
		CHECK(streams_on(fx, "beta") == 0u);
		REQUIRE(create_v5(fx, 3, "beta", &hinted) == XRT_SUCCESS);
		CHECK(streams_on(fx, "beta") == 1u); // the hint, for this create only
		CHECK(streams_on(fx, "alpha") == 1u);
		// The "<id>/<model id>" form names the provider's capability.
		uint64_t by_model = 0;
		REQUIRE(create_v5(fx, 3, "beta/fake", &by_model) == XRT_SUCCESS);
		CHECK(streams_on(fx, "beta") == 2u);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 3, plain) == XRT_SUCCESS);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 3, hinted) == XRT_SUCCESS);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 3, by_model) == XRT_SUCCESS);
	}
	conv_fake_fini(&a);
	conv_fake_fini(&b);
}

TEST_CASE("spec 5: a hint to an absent or terminal provider falls through to the default",
          "[ipc_lift_core][lift_spec5]")
{
	conv_fake_provider a, b;
	conv_fake_init(&a, "alpha");
	conv_fake_init(&b, "beta");
	depth_cap(a, "Alpha", XRT_CONV_STATE_READY);
	depth_cap(b, "Beta", XRT_CONV_STATE_ABSENT); // terminal: its model is not installed
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(add_ordered(fx, a, 50) >= 0);
		REQUIRE(add_ordered(fx, b, 100) >= 0);

		// Still listed (the app can show why), but a hint to it binds the default.
		auto c = candidates(fx, 3);
		REQUIRE(c.size() == 2u);
		CHECK(std::strcmp(c[1].provider_id, "beta") == 0);
		CHECK(c[1].state == XRT_CONV_STATE_ABSENT);

		uint64_t id1 = 0, id2 = 0, id3 = 0;
		CHECK(create_v5(fx, 3, "beta", &id1) == XRT_SUCCESS);             // terminal: never an error
		CHECK(create_v5(fx, 3, "no-such-provider", &id2) == XRT_SUCCESS); // unknown: never an error
		CHECK(create_v5(fx, 3, "", &id3) == XRT_SUCCESS);                 // empty = no hint
		CHECK(streams_on(fx, "alpha") == 3u);
		CHECK(streams_on(fx, "beta") == 0u);
		for (uint64_t id : {id1, id2, id3}) {
			CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 3, id) == XRT_SUCCESS);
		}
	}
	conv_fake_fini(&a);
	conv_fake_fini(&b);
}

TEST_CASE("spec 5: a hint never changes the policy preference", "[ipc_lift_core][lift_spec5]")
{
	conv_fake_provider a, b;
	conv_fake_init(&a, "alpha");
	conv_fake_init(&b, "beta");
	depth_cap(a, "Alpha", XRT_CONV_STATE_READY);
	depth_cap(b, "Beta", XRT_CONV_STATE_READY);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		fx.install_glue();
		REQUIRE(add_ordered(fx, a, 100) >= 0);
		REQUIRE(add_ordered(fx, b, 50) >= 0);
		set_preferred(fx, "alpha"); // the user's default outranks ProbeOrder

		uint64_t hinted = 0, plain = 0;
		REQUIRE(create_v5(fx, 4, "beta", &hinted) == XRT_SUCCESS);
		CHECK(streams_on(fx, "beta") == 1u);

		// The user's default is untouched: still alpha, and the next plain create binds it.
		auto c = candidates(fx, 4);
		REQUIRE(c.size() == 2u);
		CHECK(std::strcmp(c[0].provider_id, "alpha") == 0);
		CHECK(flag(c[0], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT));
		CHECK(flag(c[0], XRT_CONV_WIRE_CANDIDATE_SELECTED));
		CHECK_FALSE(flag(c[1], XRT_CONV_WIRE_CANDIDATE_USER_DEFAULT));
		REQUIRE(create_v5(fx, 4, nullptr, &plain) == XRT_SUCCESS);
		CHECK(streams_on(fx, "alpha") == 1u);
		CHECK(streams_on(fx, "beta") == 1u);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 4, hinted) == XRT_SUCCESS);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 4, plain) == XRT_SUCCESS);
	}
	conv_fake_fini(&a);
	conv_fake_fini(&b);
}

TEST_CASE("spec 5: a change behind the selection reaches an enumerating session as PROVIDERS_CHANGED",
          "[ipc_lift_core][lift_spec5]")
{
	conv_fake_provider a, b;
	conv_fake_init(&a, "alpha");
	conv_fake_init(&b, "beta");
	depth_cap(a, "Alpha", XRT_CONV_STATE_READY);
	depth_cap(b, "Beta", XRT_CONV_STATE_READY);
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE, /*events=*/true);
		fx.install_glue();
		REQUIRE(add_ordered(fx, a, 50) >= 0);
		REQUIRE(add_ordered(fx, b, 100) >= 0);
		auto drain = [&](bool *saw_providers) {
			bool has = true;
			while (has) {
				xrt_conv_wire_event ev{};
				REQUIRE(ipc_server_conv_v4_poll_event(fx.sc, 6, &has, &ev) == XRT_SUCCESS);
				if (has && ev.kind == XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED &&
				    saw_providers != nullptr) {
					*saw_providers = true;
				}
			}
		};
		REQUIRE(candidates(fx, 6).size() == 2u); // latches the session + records what it saw
		drain(nullptr);

		// beta (behind the selection) is switched off: the task's EFFECTIVE
		// state does not move (alpha is still READY), the candidate list does.
		conv_core_set_provider_enabled(fx.core, "beta", false);
		bool saw = false;
		REQUIRE(wait_until(
		    [&] {
			    drain(&saw);
			    return saw;
		    },
		    2000));
		REQUIRE(candidates(fx, 6).size() == 1u);
		CHECK(conv_core_task_state(fx.core, TASK) == XRT_CONV_STATE_READY);
	}
	conv_fake_fini(&a);
	conv_fake_fini(&b);
}
