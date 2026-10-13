// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P1a (#2001): the conversion core's runtime modules — host
 *         interface + in-service shim, stream lane, pipeline composer, job
 *         lane + staging, policy stages, conv_core.
 *
 * What must hold (roadmap conversion-providers.md §2.6–2.8, §3.1–3.2, §5.1–5.2;
 * discovery spec §2.3–2.5):
 *  - one call in flight per provider by default; LANES_CONCURRENT lets a job
 *    overlap converts, max_concurrent lets converts overlap, WARM_CONCURRENT
 *    lets a warm overlap converts; a serialised convert reports blocked_by_*;
 *  - lane isolation: a 3 s job costs a VIEWS_MONO stream ≤ 10 % throughput;
 *  - cancel: observed by the provider and reported within 100 ms;
 *  - provisional outputs while WARMING, refined (real) after READY;
 *  - rebinding on a terminal FAILED, signalled by `discontinuity` and
 *    FRAME_RESET on the new provider's first frame;
 *  - composition golden: a DEPTH provider + the (CPU fake) reference view
 *    synthesis serves VIEWS_MONO;
 *  - convergence handed over in DECODED units, round-tripping the app's [0,1];
 *  - the budget rule per depth kind (metric from range, affine from strength).
 */

#include "catch_amalgamated.hpp"

#include "conv_fake_provider.h"

#include "conversion/conv_core.h"
#include "conversion/conv_host_inproc.h"
#include "conversion/conv_pipeline.h"
#include "conversion/conv_policy.h"
#include "conversion/conv_stage.h"

#include "os/os_time.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t MS = 1000000ull;

bool
close_to(double a, double b, double eps)
{
	return std::fabs(a - b) <= eps;
}

uint64_t
now_ns()
{
	return os_monotonic_get_ns();
}

void
sleep_ms(uint32_t ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool
wait_until(const std::function<bool()> &pred, uint32_t timeout_ms)
{
	const uint64_t end = now_ns() + (uint64_t)timeout_ms * MS;
	while (now_ns() < end) {
		if (pred()) {
			return true;
		}
		sleep_ms(2);
	}
	return pred();
}

/*
 * CPU image allocation, view synthesis fake, event sink.
 */

uint32_t
bpp_of(uint32_t fmt)
{
	switch (fmt) {
	case XRT_CONV_FMT_R8_UNORM: return 1;
	case XRT_CONV_FMT_R16_FLOAT:
	case XRT_CONV_FMT_R16_UNORM: return 2;
	case XRT_CONV_FMT_RGBA16_FLOAT: return 8;
	default: return 4;
	}
}

std::atomic<uint64_t> g_import_key{1};
std::atomic<int> g_live_images{0};

bool
cpu_alloc(void *, const conv_image_request *req, xrt_conv_image *out)
{
	const uint32_t bpp = bpp_of(req->format);
	const uint64_t size = (uint64_t)req->width * req->height * bpp * (req->array_layers ? req->array_layers : 1);
	void *p = std::calloc(1, (size_t)size);
	if (p == nullptr) {
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->memory_kind = XRT_CONV_MEM_CPU;
	out->cpu_ptr = p;
	out->byte_size = size;
	out->width = req->width;
	out->height = req->height;
	out->format = req->format;
	out->row_stride = req->width * bpp;
	out->import_key = g_import_key++;
	out->color_space = req->role == 0 ? XRT_CONV_CS_SRGB : XRT_CONV_CS_DATA;
	g_live_images++;
	return true;
}

void
cpu_free(void *, xrt_conv_image *img)
{
	std::free(img->cpu_ptr);
	img->cpu_ptr = nullptr;
	g_live_images--;
}

struct FakeViewsyn
{
	std::mutex m;
	int calls = 0;
	conv_viewsyn_request last{};
};

bool
fake_render(void *ud, const conv_viewsyn_request *r)
{
	auto *vs = static_cast<FakeViewsyn *>(ud);
	{
		std::lock_guard<std::mutex> lk(vs->m);
		vs->calls++;
		vs->last = *r;
	}
	// View i is filled with R = i, G = 0x55 (ROW layout: N side by side).
	const xrt_conv_image *t = r->target;
	if (t->memory_kind != XRT_CONV_MEM_CPU || r->view_count == 0) {
		return false;
	}
	auto *px = static_cast<uint8_t *>(t->cpu_ptr);
	const uint32_t vw = t->width / r->view_count;
	for (uint32_t y = 0; y < t->height; y++) {
		for (uint32_t x = 0; x < t->width; x++) {
			uint8_t *p = px + (size_t)y * t->row_stride + (size_t)x * 4;
			p[0] = (uint8_t)(x / vw);
			p[1] = 0x55;
			p[2] = 0;
			p[3] = 0xFF;
		}
	}
	return true;
}

struct Events
{
	std::mutex m;
	std::vector<xrt_conv_wire_event> evs;
};

void
on_event(void *ud, uint64_t, const xrt_conv_wire_event *ev)
{
	auto *e = static_cast<Events *>(ud);
	std::lock_guard<std::mutex> lk(e->m);
	e->evs.push_back(*ev);
}

struct Core
{
	conv_core *c = nullptr;
	FakeViewsyn vs;
	Events ev;

	explicit Core(bool with_viewsyn)
	{
		conv_core_config cfg{};
		cfg.alloc.alloc = cpu_alloc;
		cfg.alloc.free = cpu_free;
		if (with_viewsyn) {
			cfg.viewsyn.render = fake_render;
			cfg.viewsyn.ud = &vs;
		}
		cfg.on_event = on_event;
		cfg.event_ud = &ev;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63; // nothing extra: only what a test creates warms
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		REQUIRE(conv_core_create(&cfg, &c) == XRT_SUCCESS);
	}
	~Core()
	{
		conv_core_destroy(&c);
	}

	int32_t
	add(conv_fake_provider &f, uint32_t probe_order)
	{
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = probe_order;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		return conv_core_add_provider(c, &d);
	}

	bool
	saw_state(uint32_t task, uint32_t state)
	{
		std::lock_guard<std::mutex> lk(ev.m);
		for (const auto &e : ev.evs) {
			if (e.kind == XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED && e.task == task && e.state == state) {
				return true;
			}
		}
		return false;
	}
};

xrt_conv_wire_stream_desc
stream_desc(uint32_t task, uint32_t outputs, uint32_t views)
{
	xrt_conv_wire_stream_desc d{};
	d.struct_size = sizeof(d);
	d.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	d.task = task;
	d.outputs = outputs;
	d.view_count = views;
	d.views_layout = XRT_CONV_VIEWS_ROW;
	d.input_kind = XRT_CONV_INPUT_MONO;
	return d;
}

bool
submit(conv_core *c,
       uint64_t sid,
       uint32_t w = 64,
       uint32_t h = 32,
       uint32_t flags = 0,
       bool conv_set = false,
       float conv = 0.0f)
{
	conv_submit_layout lay{};
	lay.image_count = 1;
	lay.width[0] = w;
	lay.height[0] = h;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	conv_submit_slot slot{};
	if (conv_core_stream_begin_submit(c, sid, &lay, &slot) != XRT_SUCCESS) {
		return false;
	}
	std::memset(slot.images[0].cpu_ptr, 0x80, (size_t)slot.images[0].byte_size); // S0: the snapshot
	conv_frame_submit fr{};
	fr.source_time = (int64_t)now_ns();
	fr.frame_flags = flags;
	fr.rect_valid = true;
	fr.rect_w_m = 0.30f;
	fr.rect_h_m = 0.20f;
	fr.convergence_set = conv_set;
	fr.convergence_rel01 = conv;
	uint64_t fid = 0;
	return conv_core_stream_commit_submit(c, sid, slot.slot, &fr, &fid) == XRT_SUCCESS;
}

conv_stream_stats
stats(conv_core *c, uint64_t sid)
{
	conv_stream_stats s{};
	REQUIRE(conv_core_stream_get_stats(c, sid, &s) == XRT_SUCCESS);
	return s;
}

/*
 * Host-level helpers.
 */

struct AsyncCmd
{
	conv_host_cmd cmd{};
	std::atomic<bool> done{false};
	std::atomic<uint64_t> done_ns{0};
};

void
async_done(conv_host_cmd *cmd)
{
	auto *a = static_cast<AsyncCmd *>(cmd->user);
	a->done_ns = now_ns();
	a->done = true;
}

struct JobBits
{
	xrt_conv_job_in in{};
	xrt_conv_params params{};
	xrt_conv_job_control ctl{};
	xrt_conv_job_out out{};
	JobBits()
	{
		in.struct_size = sizeof(in);
		params.struct_size = sizeof(params);
		ctl.struct_size = sizeof(ctl);
		ctl.is_cancelled = [](void *) { return false; };
		out.struct_size = sizeof(out);
	}
};

struct ConvertBits
{
	xrt_conv_frame_in in{};
	xrt_conv_params params{};
	xrt_conv_frame_out out{};
	ConvertBits()
	{
		in.struct_size = sizeof(in);
		params.struct_size = sizeof(params);
		out.struct_size = sizeof(out);
	}
};

uint64_t
host_stream(conv_host *h, uint32_t cap_id)
{
	xrt_conv_stream_info info{};
	info.struct_size = sizeof(info);
	info.input_kind = XRT_CONV_INPUT_MONO;
	info.view_count = 2;
	conv_host_cmd cmd{};
	cmd.op = CONV_OP_STREAM_CREATE;
	cmd.cap_id = cap_id;
	cmd.stream_info = &info;
	REQUIRE(conv_host_call(h, &cmd) == XRT_SUCCESS);
	return cmd.stream;
}

xrt_result_t
host_convert(conv_host *h, uint32_t cap_id, uint64_t stream, uint32_t *out_blocked = nullptr)
{
	ConvertBits b;
	conv_host_cmd cmd{};
	cmd.op = CONV_OP_CONVERT;
	cmd.cap_id = cap_id;
	cmd.stream = stream;
	cmd.frame_in = &b.in;
	cmd.params = &b.params;
	cmd.frame_out = &b.out;
	const xrt_result_t r = conv_host_call(h, &cmd);
	if (out_blocked != nullptr) {
		*out_blocked = cmd.blocked;
	}
	return r;
}

} // namespace


/*
 *
 * Host: the threading contract.
 *
 */

TEST_CASE("host: one call in flight by default; LANES_CONCURRENT lets a job overlap converts", "[conv_lanes]")
{
	for (const bool lanes : {false, true}) {
		CAPTURE(lanes);
		conv_fake_provider f;
		conv_fake_init(&f, "fake");
		conv_fake_cap *sc =
		    conv_fake_add_cap(&f, 1, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
		sc->convert_ms = 10;
		conv_fake_cap *jc =
		    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB,
		                      lanes ? XRT_CONV_CAP_LANES_CONCURRENT : 0);
		jc->job_ms = 300;

		conv_host *h = nullptr;
		REQUIRE(conv_host_inproc_create(f.id, &f.iface, conv_fake_inst(&f), &h) == XRT_SUCCESS);
		struct conv_host_heartbeat hb{};
		conv_host_get_heartbeat(h, &hb);
		CHECK(hb.exec_threads == (lanes ? 2u : 1u));
		const uint64_t s = host_stream(h, 1);

		AsyncCmd job;
		JobBits jb;
		job.cmd.op = CONV_OP_JOB;
		job.cmd.cap_id = 2;
		job.cmd.job_in = &jb.in;
		job.cmd.params = &jb.params;
		job.cmd.job_ctl = &jb.ctl;
		job.cmd.job_out = &jb.out;
		job.cmd.on_complete = async_done;
		job.cmd.user = &job;
		REQUIRE(conv_host_submit(h, &job.cmd) == XRT_SUCCESS);
		REQUIRE(wait_until([&] { return f.job_running; }, 1000));

		const uint64_t t0 = now_ns();
		uint32_t blocked = 0;
		REQUIRE(host_convert(h, 1, s, &blocked) == XRT_SUCCESS);
		const uint64_t dt = now_ns() - t0;
		if (lanes) {
			CHECK(dt < 150 * MS); // ran beside the job
			CHECK(blocked == 0u);
			CHECK_FALSE(job.done.load());
			CHECK(f.max_in_flight == 2u);
		} else {
			CHECK(job.done.load()); // waited for it
			CHECK((blocked & CONV_CMD_BLOCKED_BY_JOB) != 0u);
			CHECK(f.max_in_flight == 1u);
		}
		REQUIRE(wait_until([&] { return job.done.load(); }, 2000));
		conv_host_destroy(&h);
		CHECK(f.destroyed);
		conv_fake_fini(&f);
	}
}

TEST_CASE("host: max_concurrent lets converts overlap; WARM_CONCURRENT lets a warm overlap them", "[conv_lanes]")
{
	for (const uint32_t maxc : {1u, 2u}) {
		CAPTURE(maxc);
		conv_fake_provider f;
		conv_fake_init(&f, "fake");
		conv_fake_cap *c =
		    conv_fake_add_cap(&f, 1, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
		c->convert_ms = 100;
		c->cap.max_concurrent = maxc;
		conv_host *h = nullptr;
		REQUIRE(conv_host_inproc_create(f.id, &f.iface, conv_fake_inst(&f), &h) == XRT_SUCCESS);
		const uint64_t s = host_stream(h, 1);
		std::thread a([&] { host_convert(h, 1, s); });
		std::thread b([&] { host_convert(h, 1, s); });
		a.join();
		b.join();
		CHECK(f.max_in_flight == maxc);
		conv_host_destroy(&h);
		conv_fake_fini(&f);
	}
	for (const bool warm_conc : {false, true}) {
		CAPTURE(warm_conc);
		conv_fake_provider f;
		conv_fake_init(&f, "fake");
		conv_fake_cap *c =
		    conv_fake_add_cap(&f, 1, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
		c->convert_ms = 5;
		conv_fake_cap *w = conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_LANE_STREAM,
		                                     XRT_CONV_OUT_DEPTH, warm_conc ? XRT_CONV_CAP_WARM_CONCURRENT : 0);
		w->warm_ms = 300;
		conv_host *h = nullptr;
		REQUIRE(conv_host_inproc_create(f.id, &f.iface, conv_fake_inst(&f), &h) == XRT_SUCCESS);
		const uint64_t s = host_stream(h, 1);
		AsyncCmd warm;
		warm.cmd.op = CONV_OP_WARM;
		warm.cmd.cap_id = 2;
		warm.cmd.on_complete = async_done;
		warm.cmd.user = &warm;
		REQUIRE(conv_host_submit(h, &warm.cmd) == XRT_SUCCESS);
		REQUIRE(wait_until([&] { return f.warms == 1; }, 1000));
		uint32_t blocked = 0;
		REQUIRE(host_convert(h, 1, s, &blocked) == XRT_SUCCESS);
		CHECK(warm.done.load() == !warm_conc);
		CHECK(((blocked & CONV_CMD_BLOCKED_BY_WARM) != 0u) == !warm_conc);
		REQUIRE(wait_until([&] { return warm.done.load(); }, 2000));
		conv_host_destroy(&h);
		conv_fake_fini(&f);
	}
}

TEST_CASE("host: images are registered once per slot and looked up per frame", "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_add_cap(&f, 1, XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	conv_host *h = nullptr;
	REQUIRE(conv_host_inproc_create(f.id, &f.iface, conv_fake_inst(&f), &h) == XRT_SUCCESS);
	xrt_conv_image img{};
	img.memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE;
	img.handle = 0x1234;
	img.import_key = 77;
	img.width = 640;
	uint32_t idx = 0;
	REQUIRE(conv_host_register_image(h, &img, &idx) == XRT_SUCCESS);
	xrt_conv_image got{};
	REQUIRE(conv_host_lookup_image(h, idx, &got));
	CHECK(got.handle == 0x1234u); // in-service: the service's handle, unchanged
	CHECK(got.import_key == 77u);
	conv_host_unregister_image(h, idx);
	CHECK_FALSE(conv_host_lookup_image(h, idx, &got));
	conv_host_destroy(&h);
	conv_fake_fini(&f);
}


/*
 *
 * Job lane.
 *
 */

TEST_CASE("job: cancel is observed by the provider and reported within 100 ms", "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	f.cancel_poll_ms = 10;
	conv_fake_cap *jc =
	    conv_fake_add_cap(&f, 6, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB, 0);
	jc->job_ms = 5000;
	jc->state = XRT_CONV_STATE_READY;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);

		uint8_t pixels[16 * 16 * 4] = {};
		conv_job_create_info ji{};
		ji.task = XRT_CONV_TASK_SPLATS_MONO;
		ji.image_count = 1;
		ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
		ji.images[0].cpu_ptr = pixels;
		ji.images[0].width = ji.images[0].height = 16;
		ji.images[0].row_stride = 64;
		ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
		ji.params.struct_size = sizeof(ji.params);
		uint64_t j1 = 0, j2 = 0;
		REQUIRE(conv_core_job_submit(core.c, 1, &ji, &j1) == XRT_SUCCESS);
		REQUIRE(conv_core_job_submit(core.c, 1, &ji, &j2) == XRT_SUCCESS);

		conv_job_status st{};
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(core.c, j1, &st);
			    return st.status == CONV_JOB_RUNNING;
		    },
		    2000));
		// The queued one cancels at once.
		REQUIRE(conv_core_job_cancel(core.c, j2) == XRT_SUCCESS);
		conv_core_job_status(core.c, j2, &st);
		CHECK(st.status == CONV_JOB_CANCELLED);

		const uint64_t t0 = now_ns();
		REQUIRE(conv_core_job_cancel(core.c, j1) == XRT_SUCCESS);
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(core.c, j1, &st);
			    return st.status == CONV_JOB_CANCELLED;
		    },
		    1000));
		const uint64_t dt = now_ns() - t0;
		CHECK(dt <= 100 * MS);
		CHECK(f.job_cancel_seen_ns != 0u);
		CHECK(f.job_cancel_seen_ns - t0 <= 100 * MS);
		CHECK(f.jobs == 1u); // the cancelled queued job never reached the provider
	}
	conv_fake_fini(&f);
}

TEST_CASE("job: per-connection queue limit; results are chunked with no 256 MiB latch", "[conv_lanes]")
{
	conv_stage st;
	conv_stage_init(&st, 0);
	REQUIRE(conv_stage_add_output(&st, XRT_CONV_OUT_SPLAT_BLOB, nullptr, nullptr) == 0);
	uint32_t n = 0;
	xrt_conv_blob_sink *s = conv_stage_sinks(&st, &n);
	REQUIRE(n == 1u);
	const uint64_t total = 3ull * CONV_STAGE_CHUNK_BYTES + 123;
	REQUIRE(s[0].begin(s[0].ctx, XRT_CONV_BLOB_SPZ, total));
	std::vector<uint8_t> src((size_t)total);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i * 13u + 1u);
	}
	// Odd-sized writes straddle chunk boundaries.
	for (uint64_t off = 0; off < total;) {
		const uint64_t k = (std::min<uint64_t>)(777777, total - off);
		REQUIRE(s[0].write(s[0].ctx, src.data() + off, k));
		off += k;
	}
	CHECK(conv_stage_size(&st, 0) == total);
	CHECK(conv_stage_delivery(&st, 0) == XRT_CONV_WIRE_DELIVERY_SHM);
	std::vector<uint8_t> back(5000);
	const uint64_t at = CONV_STAGE_CHUNK_BYTES - 2500;
	REQUIRE(conv_stage_read(&st, 0, at, back.data(), back.size()) == back.size());
	CHECK(std::memcmp(back.data(), src.data() + at, back.size()) == 0);
	CHECK(conv_stage_read(&st, 0, total - 10, back.data(), back.size()) == 10u);
	conv_stage_fini(&st);

	// A caller-opened file receives the bytes; nothing is kept in memory.
	struct FileSink
	{
		uint64_t bytes = 0;
	} file;
	conv_stage_init(&st, 0);
	REQUIRE(conv_stage_add_output(
	            &st, XRT_CONV_OUT_SPLAT_BLOB,
	            [](void *ctx, const void *, uint64_t c) {
		            static_cast<FileSink *>(ctx)->bytes += c;
		            return true;
	            },
	            &file) == 0);
	s = conv_stage_sinks(&st, &n);
	REQUIRE(s[0].begin(s[0].ctx, XRT_CONV_BLOB_PLY_3DGS, 10));
	REQUIRE(s[0].write(s[0].ctx, src.data(), 10));
	CHECK(file.bytes == 10u);
	CHECK(conv_stage_delivery(&st, 0) == XRT_CONV_WIRE_DELIVERY_FILE);
	conv_stage_fini(&st);

	// An explicit in-memory limit refuses, never truncates silently.
	conv_stage_init(&st, 16);
	REQUIRE(conv_stage_add_output(&st, XRT_CONV_OUT_SPLAT_BLOB, nullptr, nullptr) == 0);
	s = conv_stage_sinks(&st, &n);
	REQUIRE(s[0].begin(s[0].ctx, XRT_CONV_BLOB_SPZ, 32));
	CHECK_FALSE(s[0].write(s[0].ctx, src.data(), 32));
	CHECK(conv_stage_delivery(&st, 0) == XRT_CONV_WIRE_DELIVERY_NONE);
	conv_stage_fini(&st);
}


/*
 *
 * Stream lane.
 *
 */

TEST_CASE("lane isolation: a 3 s job costs a VIEWS_MONO stream <= 10 % throughput", "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *sc =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	sc->convert_ms = 4;
	sc->state = XRT_CONV_STATE_READY;
	conv_fake_cap *jc = conv_fake_add_cap(&f, 6, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB,
	                                      XRT_CONV_OUT_SPLAT_BLOB, XRT_CONV_CAP_LANES_CONCURRENT);
	jc->job_ms = 3000;
	jc->job_bytes = 3ull * 1024 * 1024 + 17;
	jc->state = XRT_CONV_STATE_READY;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		auto d = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		CHECK((cr.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) == 0u);

		std::atomic<bool> run{true};
		std::thread producer([&] {
			while (run) {
				submit(core.c, cr.stream_id);
				sleep_ms(1);
			}
		});
		auto window = [&](uint32_t ms) {
			const uint64_t a = stats(core.c, cr.stream_id).converted;
			sleep_ms(ms);
			return stats(core.c, cr.stream_id).converted - a;
		};
		sleep_ms(300); // settle
		const uint64_t base = window(2000);

		uint8_t pixels[32 * 32 * 4] = {};
		conv_job_create_info ji{};
		ji.task = XRT_CONV_TASK_SPLATS_MONO;
		ji.image_count = 1;
		ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
		ji.images[0].cpu_ptr = pixels;
		ji.images[0].width = ji.images[0].height = 32;
		ji.images[0].row_stride = 128;
		ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
		ji.params.struct_size = sizeof(ji.params);
		uint64_t job = 0;
		REQUIRE(conv_core_job_submit(core.c, 1, &ji, &job) == XRT_SUCCESS);
		conv_job_status js{};
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(core.c, job, &js);
			    return js.status == CONV_JOB_RUNNING;
		    },
		    1000));
		const uint64_t with_job = window(2000);
		conv_core_job_status(core.c, job, &js);
		CHECK(js.status == CONV_JOB_RUNNING); // the whole window overlapped the job
		run = false;
		producer.join();

		// Progress events are rate-limited (2 Hz, 5 % steps): the provider reports every 10 ms.
		uint32_t progress_events = 0;
		{
			std::lock_guard<std::mutex> lk(core.ev.m);
			for (const auto &e : core.ev.evs) {
				if (e.kind == XRT_CONV_WIRE_EVENT_JOB_STATE_CHANGED && e.job_id == job &&
				    e.state == CONV_JOB_RUNNING) {
					progress_events++;
				}
			}
		}
		CHECK(progress_events >= 2u);
		CHECK(progress_events <= 1u + 2u * 3u);

		INFO("baseline " << base << " conversions / 2 s, with the job " << with_job);
		CHECK(base > 50u);
		CHECK((double)with_job >= 0.9 * (double)base);
		CHECK(stats(core.c, cr.stream_id).blocked_by_job == 0u);

		// The job finishes; its 3 MiB result reads back in chunks.
		REQUIRE(wait_until(
		    [&] {
			    conv_core_job_status(core.c, job, &js);
			    return js.status == CONV_JOB_SUCCEEDED;
		    },
		    3000));
		xrt_conv_wire_job_result res{};
		REQUIRE(conv_core_job_result(core.c, job, 0, &res) == XRT_SUCCESS);
		CHECK(res.byte_size == jc->job_bytes);
		CHECK(res.delivery == XRT_CONV_WIRE_DELIVERY_SHM);
		CHECK(res.blob_format == XRT_CONV_BLOB_SPZ);
		std::vector<uint8_t> chunk(4096);
		uint64_t got = 0, total = 0;
		const uint64_t off = 1024 * 1024 - 100;
		REQUIRE(conv_core_job_read(core.c, job, 0, off, chunk.data(), chunk.size(), &got, &total) ==
		        XRT_SUCCESS);
		CHECK(got == chunk.size());
		CHECK(total == jc->job_bytes);
		for (uint64_t k = 0; k < got; k++) {
			if (chunk[(size_t)k] != (uint8_t)((off + k) * 7u + 3u)) {
				FAIL("job bytes differ at " << off + k);
			}
		}
	}
	conv_fake_fini(&f);
}

TEST_CASE("stream: provisional outputs while WARMING, real after READY", "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 0);
	c->warm_ms = 400;
	c->convert_ms = 2;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		CHECK(conv_core_task_state(core.c, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_INSTALLED);
		CHECK(conv_core_v3_state(core.c, CONV_V3_MODE_SBS, false) == CONV_V3_STATE_ACTIVATING);

		auto d = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 2);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		CHECK((cr.outputs & XRT_CONV_OUT_DEPTH) != 0u);
		conv_stream_stats s = stats(core.c, cr.stream_id);
		CHECK(s.provisional);
		CHECK((s.row_state == XRT_CONV_STATE_WARMING || s.row_state == XRT_CONV_STATE_INSTALLED));

		// Frames wait (latest-wins) while warming: nothing is published.
		for (int i = 0; i < 3; i++) {
			REQUIRE(submit(core.c, cr.stream_id));
		}
		sleep_ms(100);
		conv_result r{};
		CHECK(conv_core_stream_acquire(core.c, cr.stream_id, false, &r) == XRT_ERROR_NO_IMAGE_AVAILABLE);
		CHECK(stats(core.c, cr.stream_id).provisional);
		CHECK(conv_core_v3_state(core.c, CONV_V3_MODE_SBS, false) == CONV_V3_STATE_ACTIVATING);

		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, false, &r) == XRT_SUCCESS; }, 3000));
		CHECK_FALSE(r.provisional);
		CHECK((r.written & XRT_CONV_OUT_VIEWS) != 0u);
		CHECK((r.written & XRT_CONV_OUT_DEPTH) != 0u);
		CHECK(r.depth_info.range_min > 0.0f); // S6 measured the per-frame range
		conv_core_stream_release(core.c, cr.stream_id, r.ring_slot);
		s = stats(core.c, cr.stream_id);
		CHECK_FALSE(s.provisional);
		CHECK(s.row_state == XRT_CONV_STATE_READY);
		CHECK(s.dropped >= 2u); // latest-wins while waiting
		CHECK(core.saw_state(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_STATE_WARMING));
		CHECK(core.saw_state(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_STATE_READY));
		CHECK(conv_core_v3_state(core.c, CONV_V3_MODE_SBS, false) == CONV_V3_STATE_READY);

		// READY and lacking a required output: refused (never a re-selection).
		auto bad = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_CONFIDENCE, 2);
		xrt_conv_wire_stream_created cr2{};
		CHECK(conv_core_stream_create(core.c, 1, &bad, nullptr, &cr2) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		// G2: max_views enforced at create.
		auto many = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 9);
		CHECK(conv_core_stream_create(core.c, 1, &many, nullptr, &cr2) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		REQUIRE(conv_core_stream_destroy(core.c, cr.stream_id) == XRT_SUCCESS);
	}
	CHECK(g_live_images.load() == 0);
	conv_fake_fini(&f);
}

TEST_CASE("stream: rebinding on a terminal FAILED is signalled by discontinuity", "[conv_lanes]")
{
	conv_fake_provider fa, fb;
	conv_fake_init(&fa, "provider-a");
	conv_fake_init(&fb, "provider-b");
	for (conv_fake_provider *f : {&fa, &fb}) {
		conv_fake_cap *c =
		    conv_fake_add_cap(f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
		c->state = XRT_CONV_STATE_READY;
		c->convert_ms = 2;
	}
	{
		Core core(false);
		REQUIRE(core.add(fa, 50) >= 0);
		REQUIRE(core.add(fb, 100) >= 0);
		auto d = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		CHECK(std::string(stats(core.c, cr.stream_id).provider_id) == "provider-a");

		conv_result r{};
		REQUIRE(submit(core.c, cr.stream_id));
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r) == XRT_SUCCESS; }, 2000));
		CHECK_FALSE(r.discontinuity);
		conv_core_stream_release(core.c, cr.stream_id, r.ring_slot);

		// Device loss on A: FAILED(DEVICE_LOST), terminal for selection.
		conv_fake_find(&fa, 4)->fail_converts = true;
		REQUIRE(submit(core.c, cr.stream_id));
		REQUIRE(wait_until([&] { return stats(core.c, cr.stream_id).rebinds == 1u; }, 3000));
		const conv_stream_stats s = stats(core.c, cr.stream_id);
		CHECK(std::string(s.provider_id) == "provider-b");
		CHECK(core.saw_state(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_STATE_FAILED));

		REQUIRE(submit(core.c, cr.stream_id));
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r) == XRT_SUCCESS; }, 2000));
		CHECK(r.discontinuity);
		CHECK((fb.first_frame_flags & XRT_CONV_FRAME_RESET) != 0u); // B starts with no temporal state
		conv_core_stream_release(core.c, cr.stream_id, r.ring_slot);

		REQUIRE(submit(core.c, cr.stream_id));
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r) == XRT_SUCCESS; }, 2000));
		CHECK_FALSE(r.discontinuity);
		conv_core_stream_release(core.c, cr.stream_id, r.ring_slot);
		CHECK(fb.resets_seen == 1u);

		// An app cut / seek: FRAME_RESET + discontinuity, no rebind.
		REQUIRE(conv_core_stream_reset(core.c, cr.stream_id) == XRT_SUCCESS);
		REQUIRE(submit(core.c, cr.stream_id));
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r) == XRT_SUCCESS; }, 2000));
		CHECK(r.discontinuity);
		conv_core_stream_release(core.c, cr.stream_id, r.ring_slot);
		CHECK(fb.resets_seen == 2u);
		CHECK(stats(core.c, cr.stream_id).rebinds == 1u);
	}
	conv_fake_fini(&fa);
	conv_fake_fini(&fb);
}


/*
 *
 * Pipeline composition.
 *
 */

TEST_CASE("pipeline: compose golden (stages, outputs, provisional, view limits)", "[conv_lanes]")
{
	xrt_conv_capability depth{};
	depth.cap_id = 2;
	depth.task = XRT_CONV_TASK_DEPTH_VIDEO;
	depth.lanes = XRT_CONV_LANE_STREAM;
	depth.outputs = XRT_CONV_OUT_DEPTH;
	depth.input_formats = 1u << XRT_CONV_FMT_RGBA8_UNORM;
	depth.depth_kind = XRT_CONV_DEPTH_AFFINE_INVERSE;

	conv_pipeline_request req{};
	req.task = XRT_CONV_TASK_VIEWS_MONO;
	req.input_kind = XRT_CONV_INPUT_MONO;
	req.required_outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
	req.view_count = 3;
	req.source_format = XRT_CONV_FMT_RGBA8_UNORM;
	conv_pipeline p{};
	REQUIRE(conv_pipeline_compose(&req, &depth, true, true, &p) == XRT_SUCCESS);
	CHECK(p.composed);
	CHECK(p.stages == (CONV_STAGE_S0_SNAPSHOT | CONV_STAGE_S3_POLICY | CONV_STAGE_S4_PROVIDER |
	                   CONV_STAGE_S5_VIEWSYN | CONV_STAGE_S6_DEPTH | CONV_STAGE_S8_PUBLISH));
	CHECK(p.provider_outputs == XRT_CONV_OUT_DEPTH);
	CHECK(p.outputs == (XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH));
	CHECK(p.view_count == 3u);
	CHECK(p.depth_kind == XRT_CONV_DEPTH_AFFINE_INVERSE);

	// A weave-rect NV12 source with observers: S1, S2, S7 join.
	req.weave_rect_source = true;
	req.observers = true;
	req.source_format = XRT_CONV_FMT_NV12;
	REQUIRE(conv_pipeline_compose(&req, &depth, true, true, &p) == XRT_SUCCESS);
	CHECK((p.stages & (CONV_STAGE_S1_LETTERBOX | CONV_STAGE_S2_NORMALISE | CONV_STAGE_S7_OBSERVERS)) ==
	      (CONV_STAGE_S1_LETTERBOX | CONV_STAGE_S2_NORMALISE | CONV_STAGE_S7_OBSERVERS));

	// Composed views: 2..8 whatever the depth row says.
	req.view_count = 9;
	CHECK(conv_pipeline_compose(&req, &depth, true, true, &p) == XRT_ERROR_FEATURE_NOT_SUPPORTED);

	// A direct row lacking a required output: refused when READY, provisional otherwise.
	xrt_conv_capability views{};
	views.task = XRT_CONV_TASK_VIEWS_MONO;
	views.outputs = XRT_CONV_OUT_VIEWS;
	views.min_views = 2;
	views.max_views = 4;
	req = conv_pipeline_request{};
	req.task = XRT_CONV_TASK_VIEWS_MONO;
	req.required_outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
	req.view_count = 2;
	CHECK(conv_pipeline_compose(&req, &views, false, true, &p) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	REQUIRE(conv_pipeline_compose(&req, &views, false, false, &p) == XRT_SUCCESS);
	CHECK(p.provisional);
	CHECK((p.provider_outputs & XRT_CONV_OUT_DEPTH) != 0u);
	CHECK_FALSE(conv_pipeline_refine(&p, req.required_outputs, &views)); // READY still lacks it
	views.outputs |= XRT_CONV_OUT_DEPTH;
	REQUIRE(conv_pipeline_compose(&req, &views, false, false, &p) == XRT_SUCCESS);
	CHECK(conv_pipeline_refine(&p, req.required_outputs, &views));
	CHECK_FALSE(p.provisional);
	req.view_count = 5; // above this row's max_views (G2)
	CHECK(conv_pipeline_compose(&req, &views, false, true, &p) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
}

TEST_CASE("pipeline: DEPTH provider + reference view synthesis serves VIEWS_MONO", "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "depth-only");
	conv_fake_cap *c =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	c->state = XRT_CONV_STATE_READY;
	c->cap.depth_kind = XRT_CONV_DEPTH_METRIC;
	{
		Core core(true);
		REQUIRE(core.add(f, 100) >= 0);
		xrt_conv_wire_task_desc td{};
		REQUIRE(conv_core_get_task_desc(core.c, XRT_CONV_TASK_VIEWS_MONO, &td));
		CHECK((td.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0u);
		CHECK(td.state == XRT_CONV_STATE_READY);
		CHECK(td.max_views == 8u);

		auto d = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 3);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		CHECK((cr.desc_flags & XRT_CONV_WIRE_DESC_COMPOSED) != 0u);
		CHECK(cr.view_count == 3u);
		CHECK(cr.outputs == (XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH));

		// Frame 1: the app asks for 0.25, but there is no range yet: AUTO.
		REQUIRE(submit(core.c, cr.stream_id, 64, 32, 0, true, 0.25f));
		conv_result r1{};
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r1) == XRT_SUCCESS; }, 2000));
		CHECK(f.last_params.convergence_mode == XRT_CONV_CONVERGENCE_AUTO);
		CHECK(r1.written == (XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH));
		CHECK(r1.views_info.view_count == 3u);
		CHECK(r1.depth_info.same_inference == 1u); // the views ARE this depth's
		CHECK((r1.depth_info.suggest_flags & 4u) != 0u);
		// S6 measured the ramp 1..2 m (p05 / p95 of the decoded values).
		CHECK(close_to(r1.depth_info.range_min, 1.0, 1e-5));
		CHECK(close_to(r1.depth_info.range_max, 2.0, 1e-5));
		const float p05 = r1.depth_info.range_p05, p95 = r1.depth_info.range_p95;
		CHECK(p05 > 1.0f);
		CHECK(p95 < 2.0f);
		{
			std::lock_guard<std::mutex> lk(core.vs.m);
			CHECK(core.vs.calls == 1);
			const conv_viewsyn_request &q = core.vs.last;
			CHECK(q.view_count == 3u);
			CHECK(q.layout == XRT_CONV_VIEWS_ROW);
			CHECK(close_to(q.range_near, p05, 1e-6)); // metric: smaller z = nearer
			CHECK(close_to(q.range_far, p95, 1e-6));
			// AUTO: the middle of the content in nearness.
			CHECK(close_to(q.convergence, 1.0 / (0.5 * (1.0 / p05 + 1.0 / p95)), 1e-4));
			// No range yet (frame 1): the affine rule, strength x default.
			CHECK(close_to(q.budget, CONV_POLICY_BUDGET_DEFAULT, 1e-6));
			// No eyes snapshot: the nominal pair, spread to 3 views at its spacing.
			CHECK(close_to(q.viewpoints_m[0][0], -CONV_POLICY_NOMINAL_IPD_M, 1e-6));
			CHECK(close_to(q.viewpoints_m[1][0], 0.0, 1e-6));
			CHECK(close_to(q.viewpoints_m[2][0], CONV_POLICY_NOMINAL_IPD_M, 1e-6));
			CHECK(close_to(q.viewpoints_m[1][2], U_LIFT_NOMINAL_Z_DEFAULT_M, 1e-6));
			CHECK(close_to(q.rect_width_m, 0.30, 1e-6));
		}
		// The views target holds the stage's pixels: view 2 is R = 2.
		const auto *px = static_cast<const uint8_t *>(r1.views.cpu_ptr);
		REQUIRE(px != nullptr);
		CHECK(r1.views.width == 3u * 64u);
		CHECK(px[(size_t)(2 * 64 + 5) * 4] == 2u);
		CHECK(px[1] == 0x55u);
		const xrt_conv_depth_info last = r1.depth_info;
		conv_core_stream_release(core.c, cr.stream_id, r1.ring_slot);

		// Frame 2: the plane reaches the provider in DECODED units, mapped
		// with frame 1's range, and the stage renders exactly that plane.
		REQUIRE(submit(core.c, cr.stream_id, 64, 32, 0, true, 0.25f));
		conv_result r2{};
		REQUIRE(wait_until(
		    [&] { return conv_core_stream_acquire(core.c, cr.stream_id, true, &r2) == XRT_SUCCESS; }, 2000));
		conv_core_stream_release(core.c, cr.stream_id, r2.ring_slot);
		float expect = 0.0f;
		REQUIRE(conv_policy_convergence_to_decoded(0.25f, &last, &expect));
		CHECK(close_to(expect, 1.0 / (1.0 / p05 + 0.25 * (1.0 / p95 - 1.0 / p05)), 1e-5));
		CHECK(f.last_params.convergence_mode == XRT_CONV_CONVERGENCE_DECODED);
		CHECK(close_to(f.last_params.convergence, expect, 1e-6));
		{
			std::lock_guard<std::mutex> lk(core.vs.m);
			CHECK(close_to(core.vs.last.convergence, expect, 1e-6));
			// Metric with a range: from the content (nominal 0.5 m viewer, 0.3 m rect), capped.
			const double natural = 0.063 * (1.0 / p05 - 1.0 / p95) * 0.5 / 0.3;
			CHECK(close_to(core.vs.last.budget,
			               (std::min)(natural, (double)CONV_POLICY_BUDGET_DEFAULT *
			                                       CONV_POLICY_BUDGET_METRIC_MAX_FACTOR),
			               1e-5));
		}
		CHECK(close_to(r2.depth_info.convergence_value, expect, 1e-6));
	}
	conv_fake_fini(&f);
}


/*
 *
 * Policy stages.
 *
 */

TEST_CASE("policy: convergence DECODED mapping round-trips the app's [0,1]", "[conv_lanes]")
{
	struct Case
	{
		uint32_t kind;
		float a, b;          // p05, p95
		float near_v, far_v; // expected ends
	};
	const Case cases[] = {
	    {XRT_CONV_DEPTH_METRIC, 1.0f, 4.0f, 1.0f, 4.0f},
	    {XRT_CONV_DEPTH_METRIC_INVERSE, 0.25f, 1.0f, 1.0f, 0.25f},
	    {XRT_CONV_DEPTH_AFFINE_INVERSE, 0.1f, 0.9f, 0.9f, 0.1f},
	    {XRT_CONV_DEPTH_AFFINE_DISPARITY, -2.0f, 5.0f, 5.0f, -2.0f},
	    {XRT_CONV_DEPTH_AFFINE, 2.0f, 5.0f, 2.0f, 5.0f},
	    {XRT_CONV_DEPTH_STEREO_DISPARITY_PX, 3.0f, 12.0f, 12.0f, 3.0f},
	};
	for (const Case &c : cases) {
		CAPTURE(c.kind);
		xrt_conv_depth_info di{};
		di.kind = c.kind;
		di.range_p05 = c.a;
		di.range_p95 = c.b;
		float v = 0.0f;
		REQUIRE(conv_policy_convergence_to_decoded(0.0f, &di, &v));
		CHECK(close_to(v, c.near_v, 1e-6)); // 0 = the nearest content on the glass
		REQUIRE(conv_policy_convergence_to_decoded(1.0f, &di, &v));
		CHECK(close_to(v, c.far_v, 1e-6)); // 1 = the farthest
		for (float rel : {0.0f, 0.1f, 0.25f, 0.5f, 0.8f, 1.0f}) {
			REQUIRE(conv_policy_convergence_to_decoded(rel, &di, &v));
			float back = -1.0f;
			REQUIRE(conv_policy_convergence_from_decoded(v, &di, &back));
			CHECK(close_to(back, rel, 1e-5));
		}
	}
	// Linear in nearness: metric 1..4 m at 0.5 is 1 / ((1 + 0.25) / 2) = 1.6 m.
	xrt_conv_depth_info m{};
	m.kind = XRT_CONV_DEPTH_METRIC;
	m.range_p05 = 1.0f;
	m.range_p95 = 4.0f;
	float v = 0.0f;
	REQUIRE(conv_policy_convergence_to_decoded(0.5f, &m, &v));
	CHECK(close_to(v, 1.6, 1e-6));
	// min / max are the fallback when no percentile was computed; nothing = no mapping.
	xrt_conv_depth_info mm{};
	mm.kind = XRT_CONV_DEPTH_METRIC;
	mm.range_min = 1.0f;
	mm.range_max = 4.0f;
	REQUIRE(conv_policy_convergence_to_decoded(0.5f, &mm, &v));
	CHECK(close_to(v, 1.6, 1e-6));
	xrt_conv_depth_info none{};
	CHECK_FALSE(conv_policy_convergence_to_decoded(0.5f, &none, &v));

	// Source order: app, observer, validated hint, AUTO.
	xrt_conv_depth_info last = m;
	last.suggest_flags = 1u;
	last.suggested_convergence = 3.0f;
	xrt_conv_params p{};
	conv_policy_conv_out o{};
	conv_policy_conv_in in{};
	in.last = &last;
	in.app_set = true;
	in.app_rel01 = 0.5f;
	in.observer_valid = true;
	in.observer_rel01 = 0.0f;
	conv_policy_convergence(&in, &p, &o);
	CHECK(o.source == CONV_CONVERGENCE_SRC_APP);
	CHECK(p.convergence_mode == XRT_CONV_CONVERGENCE_DECODED);
	CHECK(close_to(p.convergence, 1.6, 1e-6));
	in.app_set = false;
	conv_policy_convergence(&in, &p, &o);
	CHECK(o.source == CONV_CONVERGENCE_SRC_OBSERVER);
	CHECK(close_to(p.convergence, 1.0, 1e-6));
	in.observer_valid = false;
	conv_policy_convergence(&in, &p, &o);
	CHECK(o.source == CONV_CONVERGENCE_SRC_AUTO); // hint ignored: not corpus-validated
	CHECK(p.convergence_mode == XRT_CONV_CONVERGENCE_AUTO);
	in.hint_allowed = true;
	conv_policy_convergence(&in, &p, &o);
	CHECK(o.source == CONV_CONVERGENCE_SRC_HINT);
	CHECK(close_to(p.convergence, 3.0, 1e-6));
	in.app_set = true;
	in.last = nullptr; // no range yet: a relative plane cannot be decoded
	conv_policy_convergence(&in, &p, &o);
	CHECK(p.convergence_mode == XRT_CONV_CONVERGENCE_AUTO);
}

TEST_CASE("policy: budget from the range for METRIC kinds only; affine is strength x default", "[conv_lanes]")
{
	auto metric = [](float zn, float zf) {
		xrt_conv_depth_info d{};
		d.kind = XRT_CONV_DEPTH_METRIC;
		d.range_p05 = zn;
		d.range_p95 = zf;
		return d;
	};
	conv_policy_budget_in in{};
	in.depth_kind = XRT_CONV_DEPTH_METRIC;
	in.strength = 1.0f;
	in.rear_cap_m = 0.05f;
	in.viewer_distance_m = 0.6f;
	in.display_width_m = 0.3f;
	in.ipd_m = 0.063f;
	conv_policy_budget_out o{};

	xrt_conv_depth_info a = metric(2.0f, 4.0f);
	in.last = &a;
	conv_policy_budget(&in, &o);
	CHECK(o.source == CONV_BUDGET_SRC_METRIC_RANGE);
	CHECK(close_to(o.budget, 0.063 * (0.5 - 0.25) * 0.6 / 0.3, 1e-6)); // 0.0315
	CHECK(close_to(o.rear_cap_m, 0.05, 1e-7));
	CHECK(close_to(o.strength, o.budget / CONV_POLICY_BUDGET_DEFAULT, 1e-6));
	xrt_conv_depth_info b = metric(2.0f, 3.0f);
	in.last = &b;
	conv_policy_budget(&in, &o);
	CHECK(close_to(o.budget, 0.063 * (0.5 - 1.0 / 3.0) * 2.0, 1e-6)); // a shallower scene, less disparity
	xrt_conv_depth_info deep = metric(0.5f, 50.0f);
	in.last = &deep;
	conv_policy_budget(&in, &o);
	CHECK(close_to(o.budget, CONV_POLICY_BUDGET_DEFAULT * CONV_POLICY_BUDGET_METRIC_MAX_FACTOR, 1e-6)); // capped
	in.strength = 0.5f;
	conv_policy_budget(&in, &o);
	CHECK(close_to(o.budget, CONV_POLICY_BUDGET_DEFAULT * CONV_POLICY_BUDGET_METRIC_MAX_FACTOR * 0.5, 1e-6));
	// METRIC ignores a strength hint even when validated.
	deep.suggest_flags = 2u;
	deep.suggested_strength = 0.25f;
	in.corpus_validated = true;
	conv_policy_budget(&in, &o);
	CHECK(o.source == CONV_BUDGET_SRC_METRIC_RANGE);

	// Affine kinds: the range says nothing absolute.
	for (uint32_t kind : {XRT_CONV_DEPTH_AFFINE, XRT_CONV_DEPTH_AFFINE_INVERSE, XRT_CONV_DEPTH_AFFINE_DISPARITY,
	                      XRT_CONV_DEPTH_STEREO_DISPARITY_PX, XRT_CONV_DEPTH_UNKNOWN}) {
		CAPTURE(kind);
		in.depth_kind = kind;
		in.strength = 0.8f;
		in.corpus_validated = false;
		xrt_conv_depth_info r1{}, r2{};
		r1.kind = r2.kind = kind;
		r1.range_p05 = 0.1f;
		r1.range_p95 = 0.2f;
		r2.range_p05 = 0.0f;
		r2.range_p95 = 50.0f;
		r2.suggest_flags = 2u;
		r2.suggested_strength = 0.5f;
		in.last = &r1;
		conv_policy_budget(&in, &o);
		CHECK(o.source == CONV_BUDGET_SRC_STRENGTH);
		CHECK(close_to(o.budget, CONV_POLICY_BUDGET_DEFAULT * 0.8, 1e-7));
		in.last = &r2;
		conv_policy_budget(&in, &o);
		CHECK(close_to(o.budget, CONV_POLICY_BUDGET_DEFAULT * 0.8, 1e-7)); // hint ignored: not validated
		in.corpus_validated = true;
		conv_policy_budget(&in, &o);
		CHECK(o.source == CONV_BUDGET_SRC_VALIDATED_HINT);
		CHECK(close_to(o.budget, CONV_POLICY_BUDGET_DEFAULT * 0.8 * 0.5, 1e-7));
		CHECK(close_to(o.rear_cap_m, 0.05, 1e-7));
	}
}

TEST_CASE("policy: viewpoints — explicit rebased, tracked N eyes, nominal when stale", "[conv_lanes]")
{
	conv_policy_view_in in{};
	in.view_count = 2;
	u_lift_view_control_default(&in.vc);
	in.rect_valid = true;
	in.rect_center[0] = 0.1f;
	in.rect_w_m = 0.3f;
	in.rect_h_m = 0.2f;
	in.nominal_z_m = 0.6f;
	conv_policy_view_state vs{};
	xrt_conv_params p{};
	conv_policy_view_out o{};

	// Explicit: only rebased to the rect centre.
	in.explicit_count = 2;
	in.explicit_xyz[0][0] = 0.07f;
	in.explicit_xyz[0][2] = 0.5f;
	in.explicit_xyz[1][0] = 0.13f;
	in.explicit_xyz[1][2] = 0.5f;
	conv_policy_viewpoints(&in, &vs, &p, &o);
	CHECK(p.viewpoint_source == XRT_CONV_VIEWPOINTS_EXPLICIT);
	CHECK(p.viewpoint_count == 2u);
	CHECK(close_to(p.viewpoints_xyz[0][0], -0.03, 1e-6));
	CHECK(close_to(p.viewpoints_xyz[1][0], 0.03, 1e-6));
	CHECK(p.viewpoint_frame == XRT_CONV_VIEWPOINT_FRAME_RECT);
	CHECK(close_to(p.baseline_m, 0.06, 1e-6));

	// Stale eyes: the nominal viewer pair in front of the rect.
	in.explicit_count = 0;
	in.eyes = nullptr;
	conv_policy_viewpoints(&in, &vs, &p, &o);
	CHECK(o.nominal);
	CHECK(p.viewpoint_count == 2u);
	CHECK(close_to(p.viewpoints_xyz[1][0] - p.viewpoints_xyz[0][0], CONV_POLICY_NOMINAL_IPD_M, 1e-6));
	CHECK(close_to(p.viewpoints_xyz[0][2], 0.6, 1e-6));

	// Four tracked eyes for four views are used as they are (centre-rebased).
	conv_eyes e{};
	e.valid = e.is_tracking = true;
	e.count = 4;
	for (uint32_t i = 0; i < 4; i++) {
		e.xyz[i][0] = 0.1f + 0.03f * ((float)i - 1.5f);
		e.xyz[i][2] = 0.6f;
	}
	in.eyes = &e;
	in.view_count = 4;
	in.recenter_off = true;
	conv_policy_viewpoints(&in, &vs, &p, &o);
	CHECK_FALSE(o.nominal);
	CHECK(p.viewpoint_count == 4u);
	CHECK(close_to(p.viewpoints_xyz[0][0], -0.045, 1e-5));
	CHECK(close_to(p.viewpoints_xyz[3][0], 0.045, 1e-5));

	// A depth stream gets no viewpoints.
	in.view_count = 0;
	conv_policy_viewpoints(&in, &vs, &p, &o);
	CHECK(p.viewpoint_count == 0u);

	// The eyes slot: a retired DP reads stale.
	conv_eyes_slot slot;
	REQUIRE(conv_eyes_slot_init(&slot) == 0);
	conv_eyes got{};
	CHECK_FALSE(conv_eyes_slot_read(&slot, 0, 0, &got));
	e.timestamp_ns = 100;
	conv_eyes_slot_publish(&slot, &e);
	CHECK(conv_eyes_slot_read(&slot, 150, 0, &got));
	CHECK_FALSE(conv_eyes_slot_read(&slot, 1000, 500, &got)); // too old
	conv_eyes_slot_retire(&slot);
	CHECK_FALSE(conv_eyes_slot_read(&slot, 150, 0, &got));
	conv_eyes_slot_fini(&slot);
}

TEST_CASE("policy: observers only see same-inference depth", "[conv_lanes]")
{
	struct Obs
	{
		int seen = 0, resets = 0;
	} obs;
	static const conv_observer_ops ops = {
	    [](void *ctx) { static_cast<Obs *>(ctx)->resets++; },
	    [](void *ctx, const xrt_conv_depth_info *, const conv_depth_samples *, const float *, uint64_t) {
		    static_cast<Obs *>(ctx)->seen++;
	    },
	    nullptr,
	    nullptr,
	};
	conv_observer o{&ops, &obs};
	xrt_conv_depth_info di{};
	di.same_inference = 0;
	CHECK_FALSE(conv_policy_observe(&o, &di, nullptr, nullptr, 0));
	di.same_inference = 1;
	CHECK(conv_policy_observe(&o, &di, nullptr, nullptr, 0));
	conv_policy_observer_reset(&o);
	CHECK(obs.seen == 1);
	CHECK(obs.resets == 1);
}

TEST_CASE("core: conv_hint_interest prewarms the default tasks; nothing registered reads UNAVAILABLE", "[conv_lanes]")
{
	conv_core_config cfg{};
	cfg.alloc.alloc = cpu_alloc;
	cfg.alloc.free = cpu_free;
	cfg.prewarm_on = CONV_PREWARM_INTEREST;
	cfg.tick_ms = 10;
	conv_core *c = nullptr;
	REQUIRE(conv_core_create(&cfg, &c) == XRT_SUCCESS);
	CHECK(conv_core_v3_state(c, 0, false) == CONV_V3_STATE_UNAVAILABLE);

	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->warm_ms = 50;
	conv_fake_cap *m =
	    conv_fake_add_cap(&f, 1, XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	m->warm_ms = 50;
	conv_provider_desc d{};
	d.id = f.id;
	d.probe_order = 100;
	d.enabled = true;
	d.iface = &f.iface;
	d.inst = conv_fake_inst(&f);
	REQUIRE(conv_core_add_provider(c, &d) >= 0);
	CHECK(conv_core_task_state(c, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_INSTALLED);
	CHECK(conv_core_v3_state(c, 0, false) == CONV_V3_STATE_ACTIVATING); // never UNAVAILABLE while cold

	xrt_conv_wire_generation g0{};
	conv_core_get_generation(c, &g0);
	conv_core_hint_interest(c, 7, 0);
	REQUIRE(wait_until([&] { return conv_core_task_state(c, XRT_CONV_TASK_VIEWS_MONO) == XRT_CONV_STATE_READY; },
	                   2000));
	// Only the prewarm tasks (views-mono, depth-video) warm; depth-mono stays cold.
	CHECK(conv_core_task_state(c, XRT_CONV_TASK_DEPTH_MONO) == XRT_CONV_STATE_INSTALLED);
	xrt_conv_wire_generation g1{};
	conv_core_get_generation(c, &g1);
	CHECK(g1.status_gen > g0.status_gen);
	CHECK(conv_core_v3_state(c, CONV_V3_MODE_SBS, false) == CONV_V3_STATE_READY);
	conv_core_destroy(&c);
	CHECK(f.destroyed);
	conv_fake_fini(&f);
}


/*
 *
 * #2030: still-image dedupe and per-DP eyes.
 *
 */

namespace {

xrt_conv_wire_stream_desc
photo_desc(uint32_t task, uint32_t outputs, uint32_t views, uint32_t hint)
{
	xrt_conv_wire_stream_desc d = stream_desc(task, outputs, views);
	d.content_hint = hint;
	return d;
}

//! One frame whose pixels are all @p fill; optionally rebinding the eyes.
bool
submit_fill(conv_core *c, uint64_t sid, uint8_t fill, uint32_t flags = 0, uint64_t eyes_dp = 0, bool rebind = false)
{
	conv_submit_layout lay{};
	lay.image_count = 1;
	lay.width[0] = 64;
	lay.height[0] = 32;
	lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
	lay.color_space[0] = XRT_CONV_CS_SRGB;
	lay.role[0] = XRT_CONV_IMG_COLOR;
	conv_submit_slot slot{};
	if (conv_core_stream_begin_submit(c, sid, &lay, &slot) != XRT_SUCCESS) {
		return false;
	}
	std::memset(slot.images[0].cpu_ptr, fill, (size_t)slot.images[0].byte_size);
	conv_frame_submit fr{};
	fr.source_time = (int64_t)now_ns();
	fr.frame_flags = flags;
	fr.rect_valid = true;
	fr.rect_w_m = 0.30f;
	fr.rect_h_m = 0.20f;
	fr.eyes_dp_set = rebind;
	fr.eyes_dp = eyes_dp;
	uint64_t fid = 0;
	return conv_core_stream_commit_submit(c, sid, slot.slot, &fr, &fid) == XRT_SUCCESS;
}

//! Every committed frame has been converted, deduped, dropped or failed.
bool
drained(conv_core *c, uint64_t sid)
{
	conv_stream_stats s{};
	if (conv_core_stream_get_stats(c, sid, &s) != XRT_SUCCESS) {
		return false;
	}
	return s.converted + s.deduped + s.dropped + s.failed >= s.submitted;
}

//! Submit @p n identical frames, one at a time (none superseded).
void
submit_n(conv_core *c, uint64_t sid, uint8_t fill, int n)
{
	for (int i = 0; i < n; i++) {
		REQUIRE(submit_fill(c, sid, fill));
		REQUIRE(wait_until([&] { return drained(c, sid); }, 2000));
	}
}

conv_eyes
eyes_pair(float ipd, float z, bool tracking = true)
{
	conv_eyes e{};
	e.valid = true;
	e.is_tracking = tracking;
	e.count = 2;
	e.xyz[0][0] = -ipd * 0.5f;
	e.xyz[1][0] = ipd * 0.5f;
	e.xyz[0][2] = e.xyz[1][2] = z;
	e.timestamp_ns = now_ns();
	return e;
}

} // namespace

TEST_CASE("stream: an unchanged PHOTO input is republished, not re-converted", "[conv_lanes][dedupe]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS, XRT_CONV_CAP_EXPLICIT_VIEWPOINTS);
	c->convert_ms = 1;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		auto d = photo_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2, XRT_CONV_CONTENT_PHOTO);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		const uint64_t sid = cr.stream_id;

		// First frame converts; the identical ones after it never reach the provider.
		submit_n(core.c, sid, 0x80, 1);
		REQUIRE(wait_until([&] { return stats(core.c, sid).converted >= 1; }, 3000));
		submit_n(core.c, sid, 0x80, 30);
		conv_stream_stats s = stats(core.c, sid);
		CHECK(s.converted == 1u);
		CHECK(s.deduped == 30u);
		uint64_t provider_converts = 0;
		{
			os_mutex_lock(&f.mtx);
			provider_converts = f.converts;
			os_mutex_unlock(&f.mtx);
		}
		CHECK(provider_converts == 1u);

		// The republished result carries the newest frame id.
		conv_result r{};
		REQUIRE(conv_core_stream_acquire(core.c, sid, false, &r) == XRT_SUCCESS);
		CHECK(r.frame_id == s.submitted);
		CHECK((r.written & XRT_CONV_OUT_VIEWS) != 0u);
		conv_core_stream_release(core.c, sid, r.ring_slot);

		// A changed picture converts; so does an app reset of the same one.
		submit_n(core.c, sid, 0x81, 1);
		CHECK(stats(core.c, sid).converted == 2u);
		REQUIRE(submit_fill(core.c, sid, 0x81, XRT_CONV_FRAME_RESET));
		REQUIRE(wait_until([&] { return drained(core.c, sid); }, 2000));
		CHECK(stats(core.c, sid).converted == 3u);
		submit_n(core.c, sid, 0x81, 5);
		CHECK(stats(core.c, sid).converted == 3u);

		// New eyes are a new S3 decision: the views must follow them.
		conv_eyes e = eyes_pair(0.064f, 0.6f);
		conv_core_eyes_publish(core.c, 0x1000, &e);
		submit_n(core.c, sid, 0x81, 1);
		CHECK(stats(core.c, sid).converted == 4u);
		REQUIRE(conv_core_stream_destroy(core.c, sid) == XRT_SUCCESS);

		// A VIDEO stream of the same unchanged picture converts every frame.
		auto dv = photo_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2, XRT_CONV_CONTENT_VIDEO);
		xrt_conv_wire_stream_created cv{};
		REQUIRE(conv_core_stream_create(core.c, 1, &dv, nullptr, &cv) == XRT_SUCCESS);
		submit_n(core.c, cv.stream_id, 0x80, 1);
		REQUIRE(wait_until([&] { return stats(core.c, cv.stream_id).converted >= 1; }, 3000));
		submit_n(core.c, cv.stream_id, 0x80, 10);
		s = stats(core.c, cv.stream_id);
		CHECK(s.deduped == 0u);
		CHECK(s.converted + s.dropped == 11u);
		CHECK(s.converted >= 10u);
		REQUIRE(conv_core_stream_destroy(core.c, cv.stream_id) == XRT_SUCCESS);
	}
	CHECK(g_live_images.load() == 0);
	conv_fake_fini(&f);
}

TEST_CASE("stream: a producer content key decides for GPU inputs", "[conv_lanes][dedupe]")
{
	// The core cannot read a GPU input: without a key it always converts, with
	// one equal keys dedupe. Simulated by keying CPU frames whose pixels differ
	// (the key wins over the bytes).
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c =
	    conv_fake_add_cap(&f, 1, XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	c->convert_ms = 1;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		auto d = photo_desc(XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_OUT_DEPTH, 0, XRT_CONV_CONTENT_PHOTO);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		const uint64_t sid = cr.stream_id;
		auto keyed = [&](uint8_t fill, uint64_t key) {
			conv_submit_layout lay{};
			lay.image_count = 1;
			lay.width[0] = 64;
			lay.height[0] = 32;
			lay.format[0] = XRT_CONV_FMT_RGBA8_UNORM;
			lay.role[0] = XRT_CONV_IMG_COLOR;
			conv_submit_slot slot{};
			REQUIRE(conv_core_stream_begin_submit(core.c, sid, &lay, &slot) == XRT_SUCCESS);
			std::memset(slot.images[0].cpu_ptr, fill, (size_t)slot.images[0].byte_size);
			conv_frame_submit fr{};
			fr.content_key = key;
			uint64_t fid = 0;
			REQUIRE(conv_core_stream_commit_submit(core.c, sid, slot.slot, &fr, &fid) == XRT_SUCCESS);
			REQUIRE(wait_until([&] { return drained(core.c, sid); }, 2000));
		};
		// The first result feeds the next frame's policy (the last depth range):
		// settle on two frames, then count.
		keyed(1, 42);
		REQUIRE(wait_until([&] { return stats(core.c, sid).converted >= 1; }, 3000));
		keyed(1, 42);
		const uint64_t c1 = stats(core.c, sid).converted;
		const uint64_t d1 = stats(core.c, sid).deduped;
		keyed(2, 42);
		keyed(3, 42);
		CHECK(stats(core.c, sid).converted == c1);
		CHECK(stats(core.c, sid).deduped == d1 + 2u);
		keyed(3, 43);
		CHECK(stats(core.c, sid).converted == c1 + 1u);
		REQUIRE(conv_core_stream_destroy(core.c, sid) == XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("policy: per-DP eyes — a stream reads the DP that owns its rect", "[conv_lanes][eyes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS, XRT_CONV_CAP_EXPLICIT_VIEWPOINTS);
	c->convert_ms = 1;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		const uint64_t dp_a = 0xA000, dp_b = 0xB000;

		// The core's own read: keyed, ANY = the latest publish, retired = stale,
		// NONE = stale; unknown = ANY while one DP ever published, then stale.
		conv_eyes ea = eyes_pair(0.060f, 0.6f), eb = eyes_pair(0.080f, 0.7f), got{};
		CHECK_FALSE(conv_core_eyes_read(core.c, CONV_EYES_DP_ANY, now_ns(), &got));
		CHECK_FALSE(conv_core_eyes_read(core.c, 0xC000, now_ns(), &got)); // nothing published yet
		conv_core_eyes_publish(core.c, dp_a, &ea);
		// Single screen: a DP that has not published yet reads the panel's eyes.
		REQUIRE(conv_core_eyes_read(core.c, 0xC000, now_ns(), &got));
		CHECK(close_to(got.xyz[1][0] - got.xyz[0][0], 0.060, 1e-6));
		conv_core_eyes_publish(core.c, dp_a, &ea); // the same DP again: still single
		REQUIRE(conv_core_eyes_read(core.c, 0xC000, now_ns(), &got));
		conv_core_eyes_publish(core.c, dp_b, &eb);
		REQUIRE(conv_core_eyes_read(core.c, dp_a, now_ns(), &got));
		CHECK(close_to(got.xyz[1][0] - got.xyz[0][0], 0.060, 1e-6));
		REQUIRE(conv_core_eyes_read(core.c, dp_b, now_ns(), &got));
		CHECK(close_to(got.xyz[1][0] - got.xyz[0][0], 0.080, 1e-6));
		REQUIRE(conv_core_eyes_read(core.c, CONV_EYES_DP_ANY, now_ns(), &got));
		CHECK(close_to(got.xyz[1][0] - got.xyz[0][0], 0.080, 1e-6)); // B published last
		// Two DPs have published: an unknown key never borrows another screen's eyes.
		CHECK_FALSE(conv_core_eyes_read(core.c, 0xC000, now_ns(), &got));
		CHECK_FALSE(got.valid);
		CHECK_FALSE(conv_core_eyes_read(core.c, CONV_EYES_DP_NONE, now_ns(), &got));

		// Streams follow their binding into the provider's viewpoints.
		auto d = photo_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2, XRT_CONV_CONTENT_VIDEO);
		xrt_conv_wire_stream_created cr{};
		conv_stream_options o{};
		o.eyes_dp = dp_a;
		REQUIRE(conv_core_stream_create(core.c, 1, &d, &o, &cr) == XRT_SUCCESS);
		const uint64_t sid = cr.stream_id;
		auto baseline = [&] {
			os_mutex_lock(&f.mtx);
			const xrt_conv_params p = f.last_params;
			os_mutex_unlock(&f.mtx);
			return (double)p.viewpoints_xyz[1][0] - (double)p.viewpoints_xyz[0][0];
		};
		auto convert_once = [&](uint64_t key, bool rebind) {
			const uint64_t before = stats(core.c, sid).converted;
			REQUIRE(submit_fill(core.c, sid, 0x80, 0, key, rebind));
			REQUIRE(wait_until([&] { return stats(core.c, sid).converted > before; }, 3000));
		};
		convert_once(0, false);
		CHECK(close_to(baseline(), 0.060, 1e-5));

		// The rect moved to B's screen: the submit rebinds.
		convert_once(dp_b, true);
		CHECK(close_to(baseline(), 0.080, 1e-5));
		// A frame that does not name a DP keeps the binding.
		ea = eyes_pair(0.050f, 0.6f);
		conv_core_eyes_publish(core.c, dp_a, &ea); // A publishes last: ANY would read A
		convert_once(0, false);
		CHECK(close_to(baseline(), 0.080, 1e-5));

		// B goes away: its streams read stale (the nominal viewer), even though
		// A is still live (another screen's eyes are in another display space).
		conv_core_eyes_retire(core.c, dp_b);
		CHECK_FALSE(conv_core_eyes_read(core.c, dp_b, now_ns(), &got));
		convert_once(0, false);
		CHECK(close_to(baseline(), CONV_POLICY_NOMINAL_IPD_M, 1e-5));

		// Explicit rebinding through the API.
		REQUIRE(conv_core_stream_bind_eyes(core.c, sid, dp_a) == XRT_SUCCESS);
		convert_once(0, false);
		CHECK(close_to(baseline(), 0.050, 1e-5));
		REQUIRE(conv_core_stream_bind_eyes(core.c, sid, CONV_EYES_DP_NONE) == XRT_SUCCESS);
		convert_once(0, false);
		CHECK(close_to(baseline(), CONV_POLICY_NOMINAL_IPD_M, 1e-5));
		REQUIRE(conv_core_stream_destroy(core.c, sid) == XRT_SUCCESS);

		// More DPs than slots: the least recently published one is evicted.
		for (uint64_t k = 1; k <= CONV_CORE_MAX_DPS + 2; k++) {
			conv_eyes e = eyes_pair(0.001f * (float)k, 0.6f);
			conv_core_eyes_publish(core.c, 0x10000 * k, &e);
		}
		const uint64_t newest = 0x10000ull * (CONV_CORE_MAX_DPS + 2);
		REQUIRE(conv_core_eyes_read(core.c, newest, now_ns(), &got));
		CHECK(close_to(got.xyz[1][0] - got.xyz[0][0], 0.001 * (CONV_CORE_MAX_DPS + 2), 1e-6));
	}
	conv_fake_fini(&f);
}

#if defined(CONV_LANES_SIM_STATIC)
extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {
bool
sim_setting(const char *key, char *buf, uint32_t size)
{
	const char *v = nullptr;
	if (std::strcmp(key, "ENABLE") == 0) {
		v = "1";
	} else if (std::strcmp(key, "LATENCY_MS") == 0) {
		v = "0";
	}
	if (v == nullptr || size == 0) {
		return false;
	}
	std::snprintf(buf, size, "%s", v);
	return true;
}
} // namespace

TEST_CASE("stream: still-image dedupe with the sim provider (converted stops growing)", "[conv_lanes][dedupe][sim]")
{
	static xrt_conversion_plugin_host_iface host{};
	host.struct_size = sizeof(host);
	host.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	host.get_setting = sim_setting;
	xrt_conversion_plugin_iface *iface = nullptr;
	uint32_t ver = 0;
	REQUIRE(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &host, &iface, &ver) ==
	        XRT_SUCCESS);
	xrt_conversion_plugin_instance *inst = nullptr;
	REQUIRE(iface->probe(&inst) == XRT_SUCCESS);
	REQUIRE(inst != nullptr);

	Core core(false);
	conv_provider_desc pd{};
	pd.id = "sim-conversion";
	pd.probe_order = 250;
	pd.enabled = true;
	pd.iface = iface;
	pd.inst = inst;
	REQUIRE(conv_core_add_provider(core.c, &pd) >= 0);

	for (const uint32_t task : {(uint32_t)XRT_CONV_TASK_VIEWS_MONO, (uint32_t)XRT_CONV_TASK_DEPTH_MONO}) {
		CAPTURE(task);
		const bool views = task == XRT_CONV_TASK_VIEWS_MONO;
		auto d = photo_desc(task, views ? XRT_CONV_OUT_VIEWS : XRT_CONV_OUT_DEPTH, views ? 2u : 0u,
		                    XRT_CONV_CONTENT_PHOTO);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		const uint64_t sid = cr.stream_id;
		submit_n(core.c, sid, 0x40, 1);
		REQUIRE(wait_until([&] { return stats(core.c, sid).converted >= 1; }, 5000));
		submit_n(core.c, sid, 0x40, 2); // the first result feeds the next policy decision

		// Page rate for one second of an unchanged picture: no new conversions.
		const uint64_t c0 = stats(core.c, sid).converted;
		const uint64_t t0 = now_ns();
		int frames = 0;
		while (now_ns() - t0 < 1000 * MS) {
			submit_n(core.c, sid, 0x40, 1);
			frames++;
			sleep_ms(40); // ~24 Hz
		}
		const conv_stream_stats s = stats(core.c, sid);
		INFO("frames " << frames << " converted " << s.converted << " deduped " << s.deduped);
		CHECK(s.converted == c0);
		CHECK(s.deduped >= (uint64_t)frames);

		// The sim output is still there, under the newest frame id.
		conv_result r{};
		REQUIRE(conv_core_stream_acquire(core.c, sid, false, &r) == XRT_SUCCESS);
		CHECK(r.frame_id == s.submitted);
		CHECK((r.written & (views ? XRT_CONV_OUT_VIEWS : XRT_CONV_OUT_DEPTH)) != 0u);
		conv_core_stream_release(core.c, sid, r.ring_slot);

		// A changed picture converts.
		submit_n(core.c, sid, 0x41, 1);
		CHECK(stats(core.c, sid).converted == c0 + 1);
		REQUIRE(conv_core_stream_destroy(core.c, sid) == XRT_SUCCESS);
	}
}
#endif


/*
 *
 * #2030: CPU-memory provider results reach GPU-only consumers.
 *
 */

namespace {

std::atomic<int> g_mirror_uploads{0};

//! A "GPU" allocator for mirrors: CPU memory tagged as a shared texture.
bool
mirror_alloc(void *ud, const conv_image_request *req, xrt_conv_image *out)
{
	if (!cpu_alloc(ud, req, out)) {
		return false;
	}
	if ((req->usage & CONV_IMAGE_USAGE_MIRROR) != 0) {
		out->memory_kind = XRT_CONV_MEM_WIN32_NT_HANDLE; // what the D3D11 glue hands back
	}
	return true;
}

bool
mirror_upload(void *, const xrt_conv_image *src, const xrt_conv_image *dst)
{
	if (src->memory_kind != XRT_CONV_MEM_CPU || dst->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE ||
	    src->byte_size != dst->byte_size) {
		return false;
	}
	std::memcpy(dst->cpu_ptr, src->cpu_ptr, (size_t)src->byte_size);
	g_mirror_uploads++;
	return true;
}

} // namespace

TEST_CASE("stream: a CPU provider's views and depth are published through a GPU mirror", "[conv_lanes][mirror]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *c = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, XRT_CONV_CAP_EXPLICIT_VIEWPOINTS);
	c->convert_ms = 1;
	for (const bool with_upload : {true, false}) {
		CAPTURE(with_upload);
		g_mirror_uploads = 0;
		conv_core_config cfg{};
		cfg.alloc.alloc = mirror_alloc;
		cfg.alloc.free = cpu_free;
		cfg.alloc.upload = with_upload ? mirror_upload : nullptr;
		cfg.prewarm_on = CONV_PREWARM_FIRST_CREATE;
		cfg.prewarm_task_mask = 1ull << 63;
		cfg.rear_cap_m = -1.0f;
		cfg.tick_ms = 10;
		conv_core *core = nullptr;
		REQUIRE(conv_core_create(&cfg, &core) == XRT_SUCCESS);
		conv_provider_desc d{};
		d.id = f.id;
		d.probe_order = 100;
		d.enabled = true;
		d.iface = &f.iface;
		d.inst = conv_fake_inst(&f);
		REQUIRE(conv_core_add_provider(core, &d) >= 0);

		auto sd = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 2);
		xrt_conv_wire_stream_created cr{};
		REQUIRE(conv_core_stream_create(core, 1, &sd, nullptr, &cr) == XRT_SUCCESS);
		for (int i = 0; i < 3; i++) {
			REQUIRE(submit(core, cr.stream_id));
			REQUIRE(wait_until([&] { return drained(core, cr.stream_id); }, 3000));
		}
		conv_result r{};
		REQUIRE(conv_core_stream_acquire(core, cr.stream_id, false, &r) == XRT_SUCCESS);
		REQUIRE((r.written & XRT_CONV_OUT_VIEWS) != 0u);
		REQUIRE((r.written & XRT_CONV_OUT_DEPTH) != 0u);
		const uint32_t want_kind = with_upload ? XRT_CONV_MEM_WIN32_NT_HANDLE : XRT_CONV_MEM_CPU;
		CHECK(r.views.memory_kind == want_kind);
		CHECK(r.depth.memory_kind == want_kind);
		// The published bytes are this frame's (the fake fills views with the frame id).
		const auto *px = static_cast<const uint8_t *>(r.views.cpu_ptr);
		REQUIRE(px != nullptr);
		CHECK(px[0] == (uint8_t)(r.frame_id & 0xFF));
		CHECK(px[r.views.byte_size - 1] == (uint8_t)(r.frame_id & 0xFF));
		CHECK(g_mirror_uploads.load() == (with_upload ? 2 * (int)stats(core, cr.stream_id).converted : 0));
		conv_core_stream_release(core, cr.stream_id, r.ring_slot);
		REQUIRE(conv_core_stream_destroy(core, cr.stream_id) == XRT_SUCCESS);
		conv_core_destroy(&core);
		CHECK(g_live_images.load() == 0); // mirrors freed with their ring slots
	}
	conv_fake_fini(&f);
}

TEST_CASE("core: row activity counts bound streams and queued jobs; each change moves status_gen (#2029)",
          "[conv_lanes]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->state = XRT_CONV_STATE_READY;
	conv_fake_cap *jc =
	    conv_fake_add_cap(&f, 6, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB, 0);
	jc->state = XRT_CONV_STATE_READY;
	jc->job_ms = 5000;
	f.cancel_poll_ms = 10;
	{
		Core core(false);
		REQUIRE(core.add(f, 100) >= 0);
		auto gen = [&] {
			xrt_conv_wire_generation g{};
			conv_core_get_generation(core.c, &g);
			return g.status_gen;
		};
		auto rows = [&](uint32_t cap, uint32_t *s, uint32_t *j) {
			conv_core_row_activity(core.c, "fake", cap, false, s, j);
		};
		uint32_t s = 9, j = 9;
		rows(4, &s, &j);
		CHECK(s == 0);
		CHECK(j == 0);
		conv_core_row_activity(core.c, "nobody", 4, false, &s, &j);
		CHECK(s == 0);

		// Stream create: counted, and the generation moved.
		uint64_t g0 = gen();
		auto d = stream_desc(XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_OUT_VIEWS, 2);
		xrt_conv_wire_stream_created cr{}, cr2{};
		REQUIRE(conv_core_stream_create(core.c, 1, &d, nullptr, &cr) == XRT_SUCCESS);
		REQUIRE(conv_core_stream_create(core.c, 2, &d, nullptr, &cr2) == XRT_SUCCESS);
		CHECK(gen() > g0);
		rows(4, &s, &j);
		CHECK(s == 2);
		CHECK(j == 0);
		// The composed row over the same cap is a different row.
		conv_core_row_activity(core.c, "fake", 4, true, &s, &j);
		CHECK(s == 0);

		// Destroy: uncounted at once (asynchronous reap or not), generation moved.
		g0 = gen();
		REQUIRE(conv_core_stream_destroy(core.c, cr.stream_id) == XRT_SUCCESS);
		CHECK(gen() > g0);
		rows(4, &s, &j);
		CHECK(s == 1);
		// The connection going away drops its streams the same way.
		g0 = gen();
		conv_core_client_gone(core.c, 2);
		CHECK(gen() > g0);
		rows(4, &s, &j);
		CHECK(s == 0);

		// Jobs: queued / running count; finishing (cancel) uncounts and moves the generation.
		uint8_t pixels[16 * 16 * 4] = {};
		conv_job_create_info ji{};
		ji.task = XRT_CONV_TASK_SPLATS_MONO;
		ji.image_count = 1;
		ji.images[0].memory_kind = XRT_CONV_MEM_CPU;
		ji.images[0].cpu_ptr = pixels;
		ji.images[0].width = ji.images[0].height = 16;
		ji.images[0].row_stride = 64;
		ji.images[0].format = XRT_CONV_FMT_RGBA8_UNORM;
		ji.params.struct_size = sizeof(ji.params);
		g0 = gen();
		uint64_t j1 = 0, j2 = 0;
		REQUIRE(conv_core_job_submit(core.c, 1, &ji, &j1) == XRT_SUCCESS);
		REQUIRE(conv_core_job_submit(core.c, 1, &ji, &j2) == XRT_SUCCESS);
		CHECK(gen() > g0);
		rows(6, &s, &j);
		CHECK(s == 0);
		CHECK(j == 2);
		g0 = gen();
		REQUIRE(conv_core_job_cancel(core.c, j2) == XRT_SUCCESS);
		CHECK(gen() > g0);
		rows(6, &s, &j);
		CHECK(j == 1);
		REQUIRE(conv_core_job_cancel(core.c, j1) == XRT_SUCCESS);
		REQUIRE(wait_until(
		    [&] {
			    uint32_t ss = 0, jj = 0;
			    rows(6, &ss, &jj);
			    return jj == 0;
		    },
		    2000));
	}
	conv_fake_fini(&f);
}
