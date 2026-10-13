// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2003: the legacy DP-lift adapter (`dp-lift`,
 *         conversion/conv_provider_legacy_dp.h) and the legacy golden replay,
 *         driven through the conversion core and the v3 route with a scripted
 *         lift-thread backend (no GPU).
 *
 *  - the static describe: three capabilities, INSTALLED before any module
 *    loads (the factory is never called to describe), passing the selftest
 *    validator (VIEWS_MONO carries EXPLICIT_VIEWPOINTS), licence UNKNOWN;
 *  - state is a plain copy of module_status;
 *  - properties before the first describe ⇒ ACTIVATING;
 *  - cold boot: ACTIVATING → READY without a restart (G5), a create while
 *    ACTIVATING accepted and served (G3);
 *  - no module at the first activation ⇒ FAILED (retryable), READY after a
 *    registry change without a restart; every attempt re-resolves (G5);
 *  - splats not ready + SBS READY ⇒ READY (any, not all);
 *  - the legacy units: the policy's RELATIVE plane and the app's strength
 *    reach the module unchanged; an N-view stream hands the module the pair.
 */

#include "catch_amalgamated.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "conversion/conv_provider_legacy_dp.h"
#include "conversion/conv_readiness.h"
#include "conversion/conv_registry.h"

#include "server/ipc_server_conv.h"
#include "shared/ipc_protocol.h"

#include "os/os_time.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace {

constexpr uint64_t MS = 1000000ull;

bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = os_monotonic_get_ns() + (uint64_t)timeout_ms * MS;
	while (os_monotonic_get_ns() < end) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

/*
 * The lift thread, scripted.
 */
struct FakeLift
{
	std::mutex m;
	bool factory = true;        //!< a lift factory resolves
	bool module_present = true; //!< the next activation finds a module
	int polls_until_ready = 0;  //!< the module reports ACTIVATING for this many polls
	uint32_t module_status = 0; //!< forced module_status after activation (0 = from the script)
	uint32_t modes = XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW;
	uint32_t max_views = 4;
	//! Aux outputs the caps answer carries while ACTIVATING and once READY. The
	//! shipping plug-in advertises AUX_DEPTH only once its model is up, so a caps
	//! answer taken while ACTIVATING says "no aux depth" (#2025).
	uint32_t aux_at_activation = XRT_DP_LIFT_AUX_DEPTH;
	uint32_t aux_at_ready = XRT_DP_LIFT_AUX_DEPTH;

	// State the snapshot reports.
	bool attempted = false, have = false;
	xrt_dp_lift_caps caps{};
	uint64_t change = 0;

	// Instrumentation.
	int activations = 0, polls = 0, streams = 0, converts = 0;
	xrt_dp_lift_params last_params{};
	uint32_t last_vpf = 0;
	float last_vps[3 * XRT_CONV_MAX_VIEWS] = {};
	xrt_dp_lift_stream_info last_info{};

	// The aux depth the module reports (scripted; the defaults are the
	// shipping module's RELATIVE + INVERSE with no convergence / relief).
	uint32_t depth_units = XRT_DP_LIFT_DEPTH_RELATIVE;
	uint32_t depth_encoding = XRT_DP_LIFT_DEPTH_ENCODING_INVERSE;
	float convergence_depth = 0.0f;
	float relief_scale = 0.0f;
	float relief_offset = 0.0f;

	void
	set_state(uint32_t st)
	{
		caps.state = st;
		caps.module_status =
		    module_status != 0
		        ? module_status
		        : (st == XRT_DP_LIFT_STATE_READY ? XRT_DP_LIFT_MODULE_READY : XRT_DP_LIFT_MODULE_ACTIVATING);
		change++;
	}
};

void
fl_snapshot(void *ud, conv_dp_lift_snapshot *out)
{
	auto *f = static_cast<FakeLift *>(ud);
	std::lock_guard<std::mutex> lk(f->m);
	std::memset(out, 0, sizeof(*out));
	out->factory_present = f->factory;
	out->attempted = f->attempted;
	out->have_module = f->have;
	out->has_depth_slot = true;
	out->caps = f->caps;
	std::snprintf(out->detail, sizeof(out->detail), "%s",
	              f->have ? "" : "display processor ships no conversion module");
	out->change_count = f->change;
}

xrt_result_t
fl_activate(void *ud, bool retry)
{
	(void)retry;
	auto *f = static_cast<FakeLift *>(ud);
	std::lock_guard<std::mutex> lk(f->m);
	f->activations++;
	f->attempted = true;
	f->have = f->factory && f->module_present;
	xrt_dp_lift_caps_init(&f->caps);
	if (f->have) {
		f->caps.modes = f->modes;
		f->caps.max_views = f->max_views;
		f->caps.max_streams = 4;
		const bool ready_now = f->polls_until_ready <= 0;
		f->caps.aux_outputs = ready_now ? f->aux_at_ready : f->aux_at_activation;
		f->caps.aux_depth_semantics = XRT_DP_LIFT_DEPTH_RELATIVE;
		std::snprintf(f->caps.backend, sizeof(f->caps.backend), "fake-module");
		f->set_state(ready_now ? XRT_DP_LIFT_STATE_READY : XRT_DP_LIFT_STATE_ACTIVATING);
	} else {
		f->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
		f->change++;
	}
	return XRT_SUCCESS;
}

void
fl_poll(void *ud)
{
	auto *f = static_cast<FakeLift *>(ud);
	std::lock_guard<std::mutex> lk(f->m);
	f->polls++;
	if (f->have && f->polls_until_ready > 0 && --f->polls_until_ready == 0) {
		f->caps.aux_outputs = f->aux_at_ready; // the model is up: its aux outputs with it
		f->set_state(XRT_DP_LIFT_STATE_READY);
	}
}

xrt_result_t
fl_stream_create(void *ud, const xrt_dp_lift_stream_info *info, uint64_t *out_id)
{
	auto *f = static_cast<FakeLift *>(ud);
	std::lock_guard<std::mutex> lk(f->m);
	if (!f->have || (f->caps.modes & info->mode) == 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	f->last_info = *info;
	*out_id = (uint64_t)++f->streams;
	return XRT_SUCCESS;
}

void
fl_stream_destroy(void *, uint64_t)
{}

xrt_result_t
fl_convert(void *ud, uint64_t, conv_dp_lift_convert *io)
{
	auto *f = static_cast<FakeLift *>(ud);
	std::lock_guard<std::mutex> lk(f->m);
	f->converts++;
	f->last_params = *io->params;
	f->last_vpf = io->viewpoint_floats;
	std::memset(f->last_vps, 0, sizeof(f->last_vps));
	if (io->viewpoints != nullptr) {
		std::memcpy(f->last_vps, io->viewpoints, sizeof(float) * io->viewpoint_floats);
	}
	const xrt_conv_image *primary = io->primary_is_depth ? io->depth_target : io->views_target;
	if (primary == nullptr || primary->memory_kind != XRT_CONV_MEM_CPU || primary->cpu_ptr == nullptr) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	std::memset(primary->cpu_ptr, 0x5a, (size_t)primary->byte_size);
	io->out_w = primary->width;
	io->out_h = primary->height;
	io->out_format = primary->format;
	if (io->primary_is_depth) {
		io->depth_written = true;
	} else {
		io->views_written = true;
	}
	if (io->want_depth && io->depth_target != nullptr && io->depth_target->cpu_ptr != nullptr) {
		std::memset(io->depth_target->cpu_ptr, 0, (size_t)io->depth_target->byte_size);
		io->depth_written = true;
		io->have_depth_meta = true;
		xrt_dp_lift_depth_init(&io->depth);
		io->depth.width = io->depth_target->width;
		io->depth.height = io->depth_target->height;
		io->depth.units = f->depth_units;
		io->depth.encoding = f->depth_encoding;
		io->depth.value_scale = 1.0f;
		io->depth.same_inference = 1;
		io->depth.convergence_depth = f->convergence_depth;
		if (f->relief_scale != 0.0f) {
			io->depth.relief_valid = 1;
			io->depth.relief_scale = f->relief_scale;
			io->depth.relief_offset = f->relief_offset;
		}
	}
	return XRT_SUCCESS;
}

//! The declared outputs of one of dp-lift's rows (the registry copy).
uint32_t
row_outputs(conv_core *core, uint32_t cap_id)
{
	auto *reg = new conv_registry();
	conv_core_copy_registry(core, reg);
	uint32_t outputs = 0;
	const int32_t pi = conv_registry_find_provider(reg, CONV_DP_LIFT_ID);
	const int32_t ri = pi >= 0 ? conv_registry_find_row(reg, (uint32_t)pi, cap_id) : -1;
	if (ri >= 0) {
		outputs = reg->rows[ri].cap.outputs;
	}
	delete reg;
	return outputs;
}

//! The readiness of one of dp-lift's rows (the registry copy the status snapshot reads).
uint32_t
row_state(conv_core *core, uint32_t cap_id)
{
	auto *reg = new conv_registry();
	conv_core_copy_registry(core, reg);
	uint32_t st = XRT_CONV_STATE_UNKNOWN;
	const int32_t pi = conv_registry_find_provider(reg, CONV_DP_LIFT_ID);
	const int32_t ri = pi >= 0 ? conv_registry_find_row(reg, (uint32_t)pi, cap_id) : -1;
	if (ri >= 0) {
		st = reg->rows[ri].rd.state;
		CHECK(reg->rows[ri].rd.retryable == (st == XRT_CONV_STATE_FAILED));
	}
	delete reg;
	return st;
}

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

//! The readiness reason + pending retry deadline of one of dp-lift's rows.
void
row_reason(conv_core *core, uint32_t cap_id, uint32_t *reason, uint64_t *retry_due_ns)
{
	auto *reg = new conv_registry();
	conv_core_copy_registry(core, reg);
	*reason = 0;
	*retry_due_ns = 0;
	const int32_t pi = conv_registry_find_provider(reg, CONV_DP_LIFT_ID);
	const int32_t ri = pi >= 0 ? conv_registry_find_row(reg, (uint32_t)pi, cap_id) : -1;
	if (ri >= 0) {
		*reason = reg->rows[ri].rd.reason;
		*retry_due_ns = conv_readiness_next_deadline(&reg->rows[ri].rd);
	}
	delete reg;
}

conv_dp_lift_backend
backend_of(FakeLift &f)
{
	conv_dp_lift_backend b{};
	b.snapshot = fl_snapshot;
	b.activate = fl_activate;
	b.poll = fl_poll;
	b.stream_create = fl_stream_create;
	b.stream_destroy = fl_stream_destroy;
	b.convert = fl_convert;
	b.ud = &f;
	return b;
}

/*
 * The core + the v3 route on the CPU.
 */
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
	return true;
}

void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
}

xrt_result_t
g_import(void *, uint64_t, xrt_graphics_buffer_handle_t, bool, uint32_t, uint32_t, const conv_submit_slot *slot)
{
	return slot->image_count >= 1 && slot->images[0].cpu_ptr != nullptr ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
}

xrt_result_t
g_import_cpu(void *, uint64_t, xrt_graphics_buffer_handle_t, bool, uint32_t w, uint32_t h, uint8_t **out)
{
	*out = (uint8_t *)std::calloc(1, (size_t)w * h * 4u);
	return *out != nullptr ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
}

void
g_release(void *, xrt_graphics_buffer_handle_t, bool)
{}

xrt_result_t
g_publish(void *, uint64_t, const conv_result *r, ipc_server_conv_published *out)
{
	const xrt_conv_image &src = (r->written & XRT_CONV_OUT_VIEWS) != 0 ? r->views : r->depth;
	out->width = src.width;
	out->height = src.height;
	out->format = 28;
	out->fence_value = 1;
	return XRT_SUCCESS;
}

bool
g_export(void *, uint64_t, bool, xrt_graphics_buffer_handle_t *, uint32_t *, uint32_t *, uint32_t *)
{
	return false;
}

bool
g_fence(void *, uint64_t, xrt_graphics_sync_handle_t *)
{
	return false;
}

void
g_gone(void *, uint64_t)
{}

struct Fixture
{
	conv_core *core = nullptr;
	ipc_server_conv *sc = nullptr;
	const xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;

	explicit Fixture(uint32_t prewarm_on, bool splats_io = false)
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
		ipc_server_conv_io_ops io{};
		io.import_input = g_import;
		io.release_input = g_release;
		io.publish_result = g_publish;
		io.export_output = g_export;
		io.export_fence = g_fence;
		io.stream_gone = g_gone;
		io.import_input_cpu = splats_io ? g_import_cpu : nullptr;
		ipc_server_conv_set_io(sc, &io);
	}
	~Fixture()
	{
		ipc_server_set_conv(nullptr);
		ipc_server_conv_destroy(&sc);
		conv_core_destroy(&core); // destroys the adapter through its host
	}

	//! Register `dp-lift` as the service does (#2003): live, or cache-only (not yet described).
	int32_t
	add_dp_lift(FakeLift *f)
	{
		conv_provider_desc d{};
		d.id = CONV_DP_LIFT_ID;
		d.probe_order = CONV_DP_LIFT_PROBE_ORDER;
		d.hosting = CONV_HOSTING_IN_SERVICE;
		d.enabled = true;
		d.flags = CONV_PROV_LICENCE_EXEMPT | CONV_PROV_LEGACY_UNITS;
		if (f != nullptr) {
			conv_dp_lift_backend b = backend_of(*f);
			REQUIRE(conv_provider_legacy_dp_create(&b, &iface, &inst));
			d.iface = iface;
			d.inst = inst;
		}
		return conv_core_add_provider(core, &d);
	}

	xrt_dp_lift_caps
	properties()
	{
		xrt_dp_lift_caps caps;
		xrt_dp_lift_caps_init(&caps);
		if (ipc_server_conv_routes_properties(sc)) {
			ipc_server_conv_lift_properties(sc, &caps);
		}
		return caps;
	}

	xrt_result_t
	create(uint64_t client, uint32_t mode, uint64_t *id)
	{
		if (!ipc_server_conv_routes_create(sc, mode, 0)) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		return ipc_server_conv_lift_stream_create(sc, client, mode, 0, 1.0f, 0, 0, id);
	}

	xrt_result_t
	submit(uint64_t client, uint64_t id, float convergence = -1.0f, float strength = 1.0f, uint32_t views = 0)
	{
		ipc_arg_lift_submit a{};
		a.stream_id = id;
		a.source_time = 7;
		a.width = 32;
		a.height = 16;
		a.has_params = 1;
		a.params.convergence = convergence;
		a.params.strength = strength;
		a.params.inpaint = 1;
		a.params.view_count = views;
		uint64_t fid = 0;
		return ipc_server_conv_lift_submit(sc, client, &a, (xrt_graphics_buffer_handle_t)(intptr_t)0x10, false,
		                                   &fid);
	}

	bool
	acquire(uint64_t client, uint64_t id, ipc_lift_result *r)
	{
		bool ready = false;
		return ipc_server_conv_lift_acquire(sc, client, id, &ready, r) == XRT_SUCCESS && ready;
	}
};

} // namespace


TEST_CASE("dp-lift: static describe, INSTALLED before any module loads, selftest-valid", "[conv_dp_lift]")
{
	FakeLift f;
	conv_dp_lift_backend b = backend_of(f);
	const xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(conv_provider_legacy_dp_create(&b, &iface, &inst));
	CHECK(std::strcmp(iface->id, CONV_DP_LIFT_ID) == 0);

	uint32_t count = 0;
	REQUIRE(iface->describe_cap(inst, 0, nullptr, &count) == XRT_SUCCESS);
	REQUIRE(count == 3u);
	const uint32_t tasks[3] = {XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_TASK_VIEWS_MONO};
	for (uint32_t i = 0; i < 3; i++) {
		xrt_conv_capability c{};
		c.struct_size = (uint32_t)sizeof(c);
		REQUIRE(iface->describe_cap(inst, i, &c, nullptr) == XRT_SUCCESS);
		CHECK(c.cap_id == i + 1);
		CHECK(c.task == tasks[i]);
		CHECK(conv_cap_validate(&c) == CONV_CAP_OK); // the selftest's exit-13 rules
		CHECK(c.licence_class == XRT_CONV_LICENCE_UNKNOWN);
		CHECK(c.commercial_ok == 0u);
		CHECK((c.lanes & XRT_CONV_LANE_JOB) == 0u); // no SPLATS, no job lane
		if (c.task == XRT_CONV_TASK_VIEWS_MONO) {
			CHECK((c.flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0u);
			CHECK(c.min_views == 2u);
			CHECK(c.max_views == 2u); // conservative: SBS only until READY (#2029)
			CHECK(c.max_streams == 4u);
			CHECK((c.outputs & XRT_CONV_OUT_DEPTH) != 0u); // provisional aux depth
		}
		if (c.task == XRT_CONV_TASK_DEPTH_VIDEO) {
			CHECK((c.flags & XRT_CONV_CAP_TEMPORAL) != 0u);
		}
		// #2030: one static describe. Before any module answered, the adapter
		// says byte for byte what the headless status (target_conv_status.c)
		// says, because both read conv_dp_lift_describe_static -- except for
		// what the live DP alone knows (it carries the depth slot).
		xrt_conv_capability s{};
		s.struct_size = (uint32_t)sizeof(s);
		REQUIRE(conv_dp_lift_describe_static(i, &s));
		xrt_conv_capability live = c;
		live.flags &= ~XRT_CONV_CAP_OFFAXIS_CAMERA;
		CHECK(std::memcmp(&s, &live, sizeof(live)) == 0);
		xrt_conv_state st{};
		st.struct_size = (uint32_t)sizeof(st);
		REQUIRE(iface->get_state(inst, c.cap_id, &st) == XRT_SUCCESS);
		CHECK(st.state == XRT_CONV_STATE_INSTALLED);
	}
	CHECK(f.activations == 0); // describing never instantiates the module
	xrt_conv_capability none{};
	CHECK_FALSE(conv_dp_lift_describe_static(CONV_DP_LIFT_CAP_COUNT, &none));

	// No lift factory: ABSENT, and retryable (a plug-in swap may bring one).
	{
		std::lock_guard<std::mutex> lk(f.m);
		f.factory = false;
	}
	xrt_conv_state st{};
	st.struct_size = (uint32_t)sizeof(st);
	REQUIRE(iface->get_state(inst, CONV_DP_LIFT_CAP_VIEWS_MONO, &st) == XRT_SUCCESS);
	CHECK(st.state == XRT_CONV_STATE_ABSENT);
	CHECK(st.retryable != 0u);
	iface->destroy(inst);
}

TEST_CASE("dp-lift: state is a plain copy of module_status; refined describe", "[conv_dp_lift]")
{
	FakeLift f;
	conv_dp_lift_backend b = backend_of(f);
	const xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(conv_provider_legacy_dp_create(&b, &iface, &inst));
	const uint64_t gen0 = iface->get_generation(inst);
	REQUIRE(fl_activate(&f, false) == XRT_SUCCESS);
	CHECK(iface->get_generation(inst) != gen0); // the refined describe is a new generation

	for (uint32_t ms = XRT_DP_LIFT_MODULE_READY; ms <= XRT_DP_LIFT_MODULE_DISABLED; ms++) {
		{
			std::lock_guard<std::mutex> lk(f.m);
			f.caps.module_status = ms;
		}
		xrt_conv_state st{};
		st.struct_size = (uint32_t)sizeof(st);
		REQUIRE(iface->get_state(inst, CONV_DP_LIFT_CAP_DEPTH_VIDEO, &st) == XRT_SUCCESS);
		CHECK(st.state == ms);
	}
	// READY, but the module has no DEPTH mode: that capability is ABSENT.
	{
		std::lock_guard<std::mutex> lk(f.m);
		f.caps.module_status = XRT_DP_LIFT_MODULE_READY;
		f.caps.modes = XRT_DP_LIFT_MODE_SBS;
	}
	xrt_conv_state st{};
	st.struct_size = (uint32_t)sizeof(st);
	REQUIRE(iface->get_state(inst, CONV_DP_LIFT_CAP_DEPTH_MONO, &st) == XRT_SUCCESS);
	CHECK(st.state == XRT_CONV_STATE_ABSENT);
	REQUIRE(iface->get_state(inst, CONV_DP_LIFT_CAP_VIEWS_MONO, &st) == XRT_SUCCESS);
	CHECK(st.state == XRT_CONV_STATE_READY);
	// SBS only: two views, no NVIEW.
	xrt_conv_capability c{};
	c.struct_size = (uint32_t)sizeof(c);
	REQUIRE(iface->describe_cap(inst, 2, &c, nullptr) == XRT_SUCCESS);
	CHECK(c.max_views == 2u);
	CHECK(conv_cap_validate(&c) == CONV_CAP_OK);
	iface->destroy(inst);
}

TEST_CASE("legacy golden replay: properties before the first describe => ACTIVATING", "[conv_dp_lift]")
{
	FakeLift f;
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	// The service knows dp-lift before it could describe it (no backend yet).
	REQUIRE(fx.add_dp_lift(nullptr) >= 0);
	CHECK_FALSE(conv_core_discovery_settled(fx.core));
	xrt_dp_lift_caps p = fx.properties();
	CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING); // never UNAVAILABLE on a cold boot
	CHECK(p.modes == 0u);
	// Live: described (INSTALLED), the CONSERVATIVE modes advertised while still
	// ACTIVATING (#2029): DEPTH + SBS, never an NVIEW the module may not have.
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	p = fx.properties();
	CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING);
	CHECK(p.modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS));
	CHECK(p.max_views == 2u);
	CHECK(p.max_streams == 4u);
	CHECK(f.activations == 0);
}

TEST_CASE("legacy golden replay: cold boot ACTIVATING -> READY without a restart (G5, G3)", "[conv_dp_lift]")
{
	FakeLift f;
	f.polls_until_ready = 2; // the module warms up for ~2 s of polling
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	CHECK(fx.properties().state == XRT_DP_LIFT_STATE_ACTIVATING);

	// G3: a create while ACTIVATING is accepted (against the declared rows).
	uint64_t id = 0;
	REQUIRE(fx.create(5, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
	REQUIRE(fx.submit(5, id) == XRT_SUCCESS);
	ipc_lift_result r{};
	CHECK_FALSE(fx.acquire(5, id, &r)); // frames wait while the module warms

	REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 8000));
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(5, id);
		    return fx.acquire(5, id, &r);
	    },
	    4000));
	CHECK(r.view_count == 2u);
	CHECK(r.width == 64u); // 2 views x 32 (the adapter's predicted output desc)
	std::lock_guard<std::mutex> lk(f.m);
	CHECK(f.activations == 1);
	CHECK(f.polls >= 2);
	CHECK(f.last_info.mode == XRT_DP_LIFT_MODE_SBS);
	CHECK(f.last_params.view_count == 2u);
	CHECK(f.last_params.viewpoint_frame == XRT_DP_LIFT_VIEWPOINT_FRAME_RECT);
	CHECK(f.last_params.inpaint == 1u);
}

TEST_CASE("dp-lift: a VIEWS|DEPTH stream created while the module warms converts once READY (#2025)",
          "[conv_dp_lift]")
{
	// The lifted-rect cursor (ADR-046 Amendment 1) chains a depth request on
	// its stream, so the stream requires VIEWS|DEPTH. The shipping plug-in
	// advertises AUX_DEPTH only once its model is up; a caps answer taken while
	// ACTIVATING therefore says "no aux depth". P1a described the capability
	// from that answer and latched the stream as unsatisfied for its life.
	FakeLift f;
	f.polls_until_ready = 2;
	f.aux_at_activation = 0; // like the plug-in: no aux depth until the model is up
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	uint64_t id = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 7, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, XRT_DP_LIFT_AUX_DEPTH, 0,
	                                           &id) == XRT_SUCCESS);
	REQUIRE(fx.submit(7, id) == XRT_SUCCESS);
	ipc_lift_result r{};
	CHECK_FALSE(fx.acquire(7, id, &r)); // frames wait while the module warms
	REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 8000));
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(7, id);
		    return fx.acquire(7, id, &r);
	    },
	    4000));
	CHECK(r.view_count == 2u);
	CHECK(r.depth.valid != 0u); // the depth request was honoured, not dropped
}

TEST_CASE("dp-lift: a stream latched unsatisfied un-latches when the capability gains the output (#2025)",
          "[conv_dp_lift]")
{
	// The adversarial order: the module reports READY *without* aux depth, the
	// stream is refined against that (unsatisfied, frames dropped), and the
	// capability gains DEPTH afterwards. The core must re-refine, never latch.
	FakeLift f;
	f.polls_until_ready = 2;
	f.aux_at_activation = 0;
	f.aux_at_ready = 0; // READY without aux depth: the stream cannot be satisfied yet
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	uint64_t id = 0;
	REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 8, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, XRT_DP_LIFT_AUX_DEPTH, 0,
	                                           &id) == XRT_SUCCESS);
	REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 8000));
	REQUIRE(wait_until([&] { return (row_outputs(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO) & XRT_CONV_OUT_DEPTH) == 0; },
	                   4000));
	ipc_lift_result r{};
	// The first gate pass refines the stream against whichever describe the row
	// carries at that moment: usually the READY one without DEPTH (the stream
	// latches unsatisfied and drops), but under load the pass can land before the
	// re-describe and refine against the provisional describe that still carried
	// DEPTH (the stream converts). Both are legal; only the first exercises the
	// un-latch, so assert the drop only when the latch was observed.
	const uint64_t core_id = id & ~IPC_SERVER_CONV_STREAM_TAG;
	bool latched = false, converting = false;
	for (int i = 0; i < 80 && !latched && !converting; i++) {
		fx.submit(8, id);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		struct conv_stream_stats st{};
		if (conv_core_stream_get_stats(fx.core, core_id, &st) == XRT_SUCCESS) {
			latched = st.unsatisfied;
			converting = st.converted > 0;
		}
	}
	REQUIRE((latched || converting));
	if (latched) {
		CHECK_FALSE(fx.acquire(8, id, &r)); // dropped: READY but lacking DEPTH
	} else {
		WARN("stream refined before the row lost DEPTH; un-latch path not exercised this run");
	}
	{
		std::lock_guard<std::mutex> lk(f.m);
		f.caps.aux_outputs = XRT_DP_LIFT_AUX_DEPTH; // the capability gains the output
		f.change++;
	}
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(8, id);
		    return fx.acquire(8, id, &r);
	    },
	    6000));
	CHECK(r.view_count == 2u);
	CHECK(r.depth.valid != 0u);
}

TEST_CASE("dp-lift: provisional caps are the conservative set until READY, then refined (#2029)", "[conv_dp_lift]")
{
	// The panel shape: modes 0x7 / 4 streams while ACTIVATING, 0x3 / 32 once
	// READY. A consumer that cached the first answer believed NVIEW existed.
	FakeLift f;
	f.polls_until_ready = 2;
	f.modes = XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS; // the module has no NVIEW
	uint64_t id = 0;
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE);
		REQUIRE(fx.add_dp_lift(&f) >= 0);
		xrt_dp_lift_caps p = fx.properties();
		CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING);
		CHECK(p.modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS));
		CHECK(p.max_views == 2u);
		CHECK(p.max_streams == 4u);
		CHECK((p.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0u); // provisional aux depth stays (#2027)

		REQUIRE(fx.create(3, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
		// While the module answers caps but is still ACTIVATING: still conservative.
		REQUIRE(wait_until(
		    [&] {
			    std::lock_guard<std::mutex> lk(f.m);
			    return f.activations >= 1;
		    },
		    2000));
		p = fx.properties();
		if (p.state != XRT_DP_LIFT_STATE_READY) {
			CHECK(p.modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS));
			CHECK(p.max_views == 2u);
		}
		// READY: refined to the module's own answer -- and it never grew past it.
		REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 6000));
		p = fx.properties();
		CHECK(p.modes == (XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS));
		CHECK(p.max_views == 2u);
		CHECK(p.max_streams == 4u); // FakeLift's module answers 4
	}

	// A module that does have NVIEW gains it only at READY.
	FakeLift g;
	g.polls_until_ready = 1;
	g.max_views = 6;
	Fixture gx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(gx.add_dp_lift(&g) >= 0);
	CHECK((gx.properties().modes & XRT_DP_LIFT_MODE_NVIEW) == 0u);
	REQUIRE(gx.create(3, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
	REQUIRE(wait_until([&] { return gx.properties().max_views == 6u; }, 6000));
	CHECK(gx.properties().state == XRT_DP_LIFT_STATE_READY);
	CHECK((gx.properties().modes & XRT_DP_LIFT_MODE_NVIEW) != 0u);

	// A module that never fills module_status is read from its coarse state
	// (as its readiness is): READY refines, ACTIVATING stays conservative.
	FakeLift h;
	h.max_views = 6;
	conv_dp_lift_backend hb = backend_of(h);
	const xrt_conversion_plugin_iface *hi = nullptr;
	xrt_conversion_plugin_instance *hn = nullptr;
	REQUIRE(conv_provider_legacy_dp_create(&hb, &hi, &hn));
	REQUIRE(fl_activate(&h, false) == XRT_SUCCESS);
	auto views_max = [&] {
		xrt_conv_capability c{};
		c.struct_size = (uint32_t)sizeof(c);
		REQUIRE(hi->describe_cap(hn, 2, &c, nullptr) == XRT_SUCCESS);
		return c.max_views;
	};
	{
		std::lock_guard<std::mutex> lk(h.m);
		h.caps.module_status = XRT_DP_LIFT_MODULE_UNKNOWN;
		h.caps.state = XRT_DP_LIFT_STATE_READY;
	}
	CHECK(views_max() == 6u);
	{
		std::lock_guard<std::mutex> lk(h.m);
		h.caps.state = XRT_DP_LIFT_STATE_ACTIVATING;
	}
	CHECK(views_max() == 2u);
	hi->destroy(hn);
}

TEST_CASE("legacy golden replay: no module at first, READY after a registry change, no restart (G5)", "[conv_dp_lift]")
{
	FakeLift f;
	f.module_present = false; // the plug-in's factory works, but ships no module yet
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	uint64_t id = 0;
	REQUIRE(fx.create(6, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
	// The warm finds no module: FAILED(model), RETRYABLE (#2029: none installed
	// yet is not terminal) with the backoff armed -- the vendor module installs
	// from its own installer, after the service started.
	REQUIRE(
	    wait_until([&] { return row_state(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO) == XRT_CONV_STATE_FAILED; }, 4000));
	uint32_t reason = 0;
	uint64_t due = 0;
	row_reason(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO, &reason, &due);
	CHECK(reason == XRT_CONV_FAIL_MODEL);
	CHECK(due != 0u);                                              // the 15 s doubling backoff re-warms on its own
	CHECK(fx.properties().state == XRT_DP_LIFT_STATE_UNAVAILABLE); // honest, while nothing can serve it
	{
		std::lock_guard<std::mutex> lk(f.m);
		CHECK(f.activations == 1);
		f.module_present = true; // the module gets installed
	}
	// (An install takes far longer than the adapter's one-attempt-per-wave window.)
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	// A registry change (a plug-in / module install) retries at once; the next
	// attempt re-resolves the factory and finds the module -- no service restart.
	conv_core_provider_changed(fx.core, CONV_DP_LIFT_ID);
	uint64_t id2 = 0;
	REQUIRE(wait_until([&] { return fx.create(6, XRT_DP_LIFT_MODE_SBS, &id2) == XRT_SUCCESS; }, 2000));
	REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 6000));
	std::lock_guard<std::mutex> lk(f.m);
	CHECK(f.activations >= 2);
}

TEST_CASE("dp-lift: the kill switch is terminal ABSENT(policy) -- v3 UNAVAILABLE, the module never activated (#2029)",
          "[conv_dp_lift]")
{
	// DXR_LIFT=0 in the service env. Before #2029 the lift thread answered "no
	// module", the core FAILED(model) it and re-warmed every 15-30 s, and
	// `lift caps` sat at ACTIVATING indefinitely.
	set_env("DXR_LIFT", "0");
	FakeLift f;
	{
		Fixture fx(CONV_PREWARM_INTEREST);
		REQUIRE(fx.add_dp_lift(&f) >= 0);
		ipc_server_conv_hint_interest(fx.sc, 4, 0); // the session's prewarm
		for (uint32_t cap :
		     {CONV_DP_LIFT_CAP_DEPTH_MONO, CONV_DP_LIFT_CAP_DEPTH_VIDEO, CONV_DP_LIFT_CAP_VIEWS_MONO}) {
			REQUIRE(wait_until([&] { return row_state(fx.core, cap) == XRT_CONV_STATE_ABSENT; }, 4000));
			uint32_t reason = 0;
			uint64_t due = 0;
			row_reason(fx.core, cap, &reason, &due);
			CHECK(reason == CONV_REASON_POLICY); // configuration, not a missing module
			CHECK(due == 0u);                    // no backoff timer: nothing re-warms on its own
		}
		// The adapter's own answer: DISABLED, not retryable.
		xrt_conv_state st{};
		st.struct_size = (uint32_t)sizeof(st);
		REQUIRE(fx.iface->get_state(fx.inst, CONV_DP_LIFT_CAP_VIEWS_MONO, &st) == XRT_SUCCESS);
		CHECK(st.state == XRT_CONV_STATE_DISABLED);
		CHECK(st.retryable == 0u);

		CHECK(conv_core_discovery_settled(fx.core));
		CHECK(fx.properties().state == XRT_DP_LIFT_STATE_UNAVAILABLE);
		CHECK(conv_core_v3_state(fx.core, 0, false) == CONV_V3_STATE_UNAVAILABLE);
		uint64_t id = 0;
		CHECK(fx.create(4, XRT_DP_LIFT_MODE_SBS, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the control thread ticks every 10 ms
		CHECK(row_state(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO) == XRT_CONV_STATE_ABSENT);
		{
			std::lock_guard<std::mutex> lk(f.m);
			CHECK(f.activations == 0); // the kill switch never instantiates the module
		}

		// The switch lifted + a registry change: back without a restart.
		set_env("DXR_LIFT", nullptr);
		conv_core_provider_changed(fx.core, CONV_DP_LIFT_ID);
		REQUIRE(wait_until([&] { return fx.properties().state == XRT_DP_LIFT_STATE_READY; }, 6000));
	}
	set_env("DXR_LIFT", nullptr);
}

TEST_CASE("dp-lift: a module that itself answers ABSENT is terminal ABSENT(model) (#2029)", "[conv_dp_lift]")
{
	FakeLift f;
	f.module_status = XRT_DP_LIFT_MODULE_ABSENT; // the module is there and says its model is not
	Fixture fx(CONV_PREWARM_INTEREST);
	REQUIRE(fx.add_dp_lift(&f) >= 0);
	ipc_server_conv_hint_interest(fx.sc, 4, 0);
	REQUIRE(
	    wait_until([&] { return row_state(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO) == XRT_CONV_STATE_ABSENT; }, 4000));
	uint32_t reason = 0;
	uint64_t due = 0;
	row_reason(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO, &reason, &due);
	CHECK(reason == XRT_CONV_FAIL_MODEL); // never labelled "policy"
	CHECK(due == 0u);
	CHECK(fx.properties().state == XRT_DP_LIFT_STATE_UNAVAILABLE);
	int activations = 0;
	{
		std::lock_guard<std::mutex> lk(f.m);
		activations = f.activations;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK(row_state(fx.core, CONV_DP_LIFT_CAP_VIEWS_MONO) == XRT_CONV_STATE_ABSENT);
	std::lock_guard<std::mutex> lk(f.m);
	CHECK(f.activations == activations); // no retry loop
}

TEST_CASE("legacy golden replay: splats not ready + SBS READY -> READY", "[conv_dp_lift]")
{
	FakeLift f;
	conv_fake_provider sp;
	conv_fake_init(&sp, "fake-splats");
	conv_fake_cap *c =
	    conv_fake_add_cap(&sp, 7, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB, 0);
	c->warm_ms = 60000; // still warming / downloading for the whole test
	{
		Fixture fx(CONV_PREWARM_FIRST_CREATE, /*splats_io*/ true);
		REQUIRE(fx.add_dp_lift(&f) >= 0);
		conv_provider_desc d{};
		d.id = sp.id;
		d.probe_order = 200;
		d.enabled = true;
		d.iface = &sp.iface;
		d.inst = conv_fake_inst(&sp);
		REQUIRE(conv_core_add_provider(fx.core, &d) >= 0);
		conv_fake_set_state(&sp, 7, XRT_CONV_STATE_DOWNLOADING, 0, false);
		(void)conv_core_warm_task(fx.core, XRT_CONV_TASK_SPLATS_MONO);

		uint64_t id = 0;
		REQUIRE(fx.create(8, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS); // warms dp-lift
		REQUIRE(wait_until(
		    [&] { return conv_core_task_state(fx.core, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_READY; },
		    4000));
		CHECK(conv_core_task_state(fx.core, XRT_CONV_TASK_SPLATS_MONO) != XRT_CONV_STATE_READY);
		// "Any", not "all": the splat model not being ready holds nothing back.
		CHECK(conv_core_v3_state(fx.core, 0, true) == CONV_V3_STATE_READY);
		const xrt_dp_lift_caps p = fx.properties();
		CHECK(p.state == XRT_DP_LIFT_STATE_READY);
		CHECK((p.modes & XRT_DP_LIFT_MODE_GAUSSIANS) != 0u); // the facade advertises it while it waits
		CHECK((p.modes & XRT_DP_LIFT_MODE_SBS) != 0u);
		CHECK(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_GAUSSIANS, 1));
	}
	conv_fake_fini(&sp);
}

TEST_CASE("dp-lift: the legacy units reach the module unchanged; N views get the pair", "[conv_dp_lift]")
{
	FakeLift f;
	f.max_views = 8;
	Fixture fx(CONV_PREWARM_FIRST_CREATE);
	REQUIRE(fx.add_dp_lift(&f) >= 0);

	// SBS, the app's plane and strength: the RELATIVE plane, not decoded units.
	uint64_t id = 0;
	REQUIRE(fx.create(9, XRT_DP_LIFT_MODE_SBS, &id) == XRT_SUCCESS);
	ipc_lift_result r{};
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(9, id, 0.3f, 1.7f);
		    return fx.acquire(9, id, &r);
	    },
	    4000));
	{
		std::lock_guard<std::mutex> lk(f.m);
		CHECK(f.last_params.convergence == Catch::Approx(0.3f));
		CHECK(f.last_params.strength == Catch::Approx(1.7f));
		CHECK(f.last_vpf == 6u); // the (nominal) pair
	}
	// AUTO stays AUTO (the module's own estimate) without an observer.
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(9, id, -1.0f, 1.0f);
		    std::lock_guard<std::mutex> lk(f.m);
		    return f.last_params.convergence < 0.0f;
	    },
	    4000));

	// NVIEW x4: the policy spreads four viewpoints; the module gets the pair.
	// (NVIEW is advertised once the READY module's caps refined the describe.)
	REQUIRE(wait_until([&] { return fx.properties().max_views == 8u; }, 2000));
	uint64_t nid = 0;
	REQUIRE(fx.create(9, XRT_DP_LIFT_MODE_NVIEW, &nid) == XRT_SUCCESS);
	REQUIRE(wait_until(
	    [&] {
		    fx.submit(9, nid, -1.0f, 1.0f, 4);
		    return fx.acquire(9, nid, &r);
	    },
	    4000));
	CHECK(r.view_count == 4u);
	std::lock_guard<std::mutex> lk(f.m);
	CHECK(f.last_params.view_count == 4u);
	CHECK(f.last_vpf == 6u);
	const float dx = f.last_vps[3] - f.last_vps[0];
	CHECK(f.last_params.baseline_m == Catch::Approx(std::fabs(dx)).margin(1e-5)); // the pair's own distance
	CHECK(f.last_params.baseline_m > 0.0f);
}

TEST_CASE("dp-lift: convergence is the DECODED value; spec 3 and spec 4 read it per kind", "[conv_dp_lift]")
{
	// The module reports the depth on the screen plane (convergence_depth);
	// the ABI carries the decoded value (1 / depth for an INVERSE encoding),
	// spec 3's convergenceDepth is the depth reading of it again, and spec
	// 4's XrLiftDepthInfoDXR::convergenceValue is the decoded value. Relief
	// is copied: it is affine in q under both conventions.
	struct Row
	{
		uint32_t units, encoding, kind;
	};
	const Row rows[] = {
	    {XRT_DP_LIFT_DEPTH_METRIC, XRT_DP_LIFT_DEPTH_ENCODING_LINEAR, XRT_CONV_DEPTH_METRIC},
	    {XRT_DP_LIFT_DEPTH_METRIC, XRT_DP_LIFT_DEPTH_ENCODING_INVERSE, XRT_CONV_DEPTH_METRIC_INVERSE},
	    {XRT_DP_LIFT_DEPTH_RELATIVE, XRT_DP_LIFT_DEPTH_ENCODING_INVERSE, XRT_CONV_DEPTH_AFFINE_INVERSE},
	    {XRT_DP_LIFT_DEPTH_RELATIVE, XRT_DP_LIFT_DEPTH_ENCODING_LINEAR, XRT_CONV_DEPTH_AFFINE},
	};
	for (const Row &row : rows) {
		CAPTURE(row.kind);
		const bool inverse = row.encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE;
		FakeLift f;
		f.depth_units = row.units;
		f.depth_encoding = row.encoding;
		f.convergence_depth = 2.0f;
		f.relief_scale = 0.07f;
		f.relief_offset = -0.02f;
		Fixture fx(CONV_PREWARM_INTEREST);
		REQUIRE(fx.add_dp_lift(&f) >= 0);
		// The create warms the module (FIRST-create semantics of the fixture).

		uint64_t id = 0;
		REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 3, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, XRT_DP_LIFT_AUX_DEPTH, 0,
		                                           &id) == XRT_SUCCESS);
		ipc_lift_result r{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(3, id);
			    return fx.acquire(3, id, &r) && r.depth.valid != 0;
		    },
		    10000));
		// Spec 3 is unchanged: the module's own depth comes back.
		CHECK(r.depth.convergence_depth == Catch::Approx(2.0f));
		CHECK(r.depth.encoding == row.encoding);
		CHECK(r.depth.units == row.units);

		// Spec 4: the decoded value and the relief, as the ABI carries them.
		bool ready = false, disc = false;
		ipc_lift_result r3{};
		xrt_conv_wire_result r4{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(3, id);
			    return ipc_server_conv_v4_acquire(fx.sc, 3, id, &ready, &disc, &r3, &r4) == XRT_SUCCESS && ready &&
			           (r4.written & XRT_CONV_OUT_DEPTH) != 0;
		    },
		    4000));
		CHECK(r4.depth_info.kind == row.kind);
		CHECK((r4.depth_info.suggest_flags & 4u) != 0u);
		CHECK(r4.depth_info.convergence_value == Catch::Approx(inverse ? 0.5f : 2.0f));
		CHECK(r4.depth_info.relief_valid == 1u);
		CHECK(r4.depth_info.relief_scale == Catch::Approx(0.07f));
		CHECK(r4.depth_info.relief_offset == Catch::Approx(-0.02f));
	}
}
