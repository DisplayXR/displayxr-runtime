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
#include "shared/ipc_protocol.h"

#include "xrt/xrt_conv_wire.h"
#include "xrt/xrt_display_status.h"
#include "xrt/xrt_limits.h"

#include "os/os_time.h"

#include <atomic>
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

	explicit Fixture(uint32_t prewarm_on)
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		cfg.prewarm_on = prewarm_on;
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
	 *  routes, else the legacy path (here: no display-processor module, the
	 *  shipping "modes 0, UNAVAILABLE"). */
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

	//! ipc_handle_lift_stream_create: the core when it routes, else the legacy answer.
	xrt_result_t
	create(uint64_t client, uint32_t mode, uint64_t *id, uint32_t aux = 0)
	{
		if (!ipc_server_conv_routes_create(sc, mode, 0)) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED; // svc_lift with no module
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
		// GAUSSIANS never routes, glue or not.
		fx.install_glue();
		CHECK_FALSE(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_GAUSSIANS, 1));
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
		CHECK_FALSE(ipc_server_conv_owns_stream(fx.sc, 1)); // a svc_lift id falls through

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
