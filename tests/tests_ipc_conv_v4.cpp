// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2054 (ADR-053 P3): XR_DXR_lift spec 4 on the service side — the
 *         conv_* adapter (server/ipc_server_conv.h) driven with the CPU fake
 *         provider, plus the app-side translation (oxr_conv_map.h).
 *
 * Pinned here:
 *  - the create rules of roadmap §7.4 (FEATURE_UNSUPPORTED / VALIDATION_FAILURE
 *    / LIMIT_REACHED; this family carries VALIDATION_FAILURE as
 *    XRT_ERROR_INPUT_UNSUPPORTED);
 *  - task events: opt-in per connection (a v3-only connection never gets
 *    one), ordered (each previousState is the last delivered state) and
 *    deduplicated (a non-selected provider's transition is not news);
 *  - ProvidersChanged on a simulated install, UNAVAILABLE -> READY without a
 *    restart;
 *  - the depth-kind contract (task depthKinds, the v3 reading, XrLiftDepthInfoDXR).
 *
 * The four v3-observable shifts are tests_lift_v3_shifts.cpp.
 */

#include "ipc_conv_v4_fixture.hpp"

#include "oxr_conv_map.h"

#include <cmath>

// The service's output-flag vocabulary is the app's (one table, two sides).
static_assert(IPC_SERVER_CONV_OUTPUT_AUX_DEPTH == XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR, "AUX_DEPTH");
static_assert(IPC_SERVER_CONV_OUTPUT_METRIC == XR_LIFT_OUTPUT_METRIC_BIT_DXR, "METRIC");
static_assert(IPC_SERVER_CONV_OUTPUT_INTRINSICS == XR_LIFT_OUTPUT_INTRINSICS_BIT_DXR, "INTRINSICS");
static_assert(IPC_SERVER_CONV_OUTPUT_TEMPORAL == XR_LIFT_OUTPUT_TEMPORAL_BIT_DXR, "TEMPORAL");
static_assert(IPC_SERVER_CONV_OUTPUT_EXPLICIT_VIEWPOINTS == XR_LIFT_OUTPUT_EXPLICIT_VIEWPOINTS_BIT_DXR, "EXPLICIT");
static_assert(IPC_SERVER_CONV_OUTPUT_CONVERGENCE_HINT == XR_LIFT_OUTPUT_CONVERGENCE_HINT_BIT_DXR, "CONV_HINT");
static_assert(IPC_SERVER_CONV_OUTPUT_CONFIDENCE == XR_LIFT_OUTPUT_CONFIDENCE_BIT_DXR, "CONFIDENCE");
static_assert(IPC_SERVER_CONV_OUTPUT_COMPOSED == XR_LIFT_OUTPUT_COMPOSED_BIT_DXR, "COMPOSED");
static_assert(IPC_SERVER_CONV_IMAGE_KIND_D3D11_NT == XR_LIFT_IMAGE_KIND_D3D11_NT_DXR, "D3D11_NT");
static_assert(IPC_SERVER_CONV_IMAGE_KIND_D3D11_DXGI == XR_LIFT_IMAGE_KIND_D3D11_DXGI_DXR, "D3D11_DXGI");



TEST_CASE("v4 create rules (roadmap 7.4)", "[ipc_conv_v4]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 0);
	v->cap.max_views = 6;
	v->cap.max_streams = 2;
	v->cap.depth_kind = XRT_CONV_DEPTH_AFFINE_DISPARITY; // not metric
	conv_fake_cap *s =
	    conv_fake_add_cap(&f, 6, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB, 0);
	(void)s;
	conv_fake_set_state(&f, 4, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx;
		uint64_t id = 0;
		// No GPU glue yet: no route, a plain FEATURE_UNSUPPORTED.
		REQUIRE(fx.add(f) >= 0);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		fx.install_glue();

		// VALIDATION_FAILURE.
		CHECK(fx.create_task(1, XRT_CONV_TASK_SPLATS_VIDEO, &id) == XRT_ERROR_INPUT_UNSUPPORTED); // reserved
		CHECK(fx.create_task(1, XRT_CONV_TASK_INPAINT, &id) == XRT_ERROR_INPUT_UNSUPPORTED);      // reserved
		CHECK(fx.create_task(1, 0, &id) == XRT_ERROR_INPUT_UNSUPPORTED);
		CHECK(fx.create_task(1, 40, &id) == XRT_ERROR_INPUT_UNSUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_DISPARITY_STEREO, &id) == XRT_ERROR_INPUT_UNSUPPORTED); // spec 5
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_FROM_DEPTH, &id) == XRT_ERROR_INPUT_UNSUPPORTED); // spec 5
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 7) ==
		      XRT_ERROR_INPUT_UNSUPPORTED); // > maxViews 6
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 9) == XRT_ERROR_INPUT_UNSUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 1) == XRT_ERROR_INPUT_UNSUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, 0, XRT_DP_LIFT_MODE_DEPTH) ==
		      XRT_ERROR_INPUT_UNSUPPORTED); // mode does not match the task
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, 0, 0, XRT_CONV_INPUT_STEREO_PAIR) ==
		      XRT_ERROR_INPUT_UNSUPPORTED); // input kind no candidate offers
		CHECK(
		    fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, 0, 0, XRT_CONV_INPUT_MONO, XRT_CONV_MEM_CPU) ==
		    XRT_ERROR_INPUT_UNSUPPORTED); // export kind no candidate offers

		// FEATURE_UNSUPPORTED: no candidate; a job-only task; an unmet
		// requiredOutputs bit on a READY provider.
		CHECK(fx.create_task(1, XRT_CONV_TASK_DEPTH_VIDEO, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_SPLATS_MONO, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, IPC_SERVER_CONV_OUTPUT_METRIC) ==
		      XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, IPC_SERVER_CONV_OUTPUT_CONFIDENCE) ==
		      XRT_ERROR_FEATURE_NOT_SUPPORTED);

		// Accepted: SBS-shaped, 4 views with the matching legacy mode, aux depth required.
		uint64_t a = 0, b = 0, c = 0;
		REQUIRE(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &a, 0, IPC_SERVER_CONV_OUTPUT_AUX_DEPTH) ==
		        XRT_SUCCESS);
		CHECK((a & IPC_SERVER_CONV_STREAM_TAG) != 0u);
		CHECK(ipc_server_conv_owns_stream(fx.sc, a)); // the v3 per-stream calls serve it
		uint64_t core_id = 0;
		uint32_t mode = 0;
		REQUIRE(ipc_server_conv_resolve_stream(fx.sc, 1, a, &core_id, &mode));
		CHECK(mode == XRT_DP_LIFT_MODE_SBS); // a weave rect may lift through it
		REQUIRE(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &b, 4, 0, XRT_DP_LIFT_MODE_NVIEW) == XRT_SUCCESS);
		REQUIRE(ipc_server_conv_resolve_stream(fx.sc, 1, b, &core_id, &mode));
		CHECK(mode == XRT_DP_LIFT_MODE_NVIEW);

		// LIMIT_REACHED: the task's provider allows 2.
		CHECK(fx.create_task(2, XRT_CONV_TASK_VIEWS_MONO, &c) == XRT_ERROR_CLIENT_LIMIT_REACHED);
		CHECK(ipc_server_conv_lift_stream_destroy(fx.sc, 1, a) == XRT_SUCCESS);
		CHECK(fx.create_task(2, XRT_CONV_TASK_VIEWS_MONO, &c) == XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v4 create: requiredOutputs refuse only a READY provider; terminal states refuse", "[ipc_conv_v4]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->warm_ms = 5000; // stays WARMING for the whole test
	conv_fake_cap *d =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	d->state = XRT_CONV_STATE_FAILED;
	d->failure_reason = XRT_CONV_FAIL_MODEL;
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		uint64_t id = 0;
		// Not READY: the declared outputs may be provisional, so no refusal.
		CHECK(fx.task(1, XRT_CONV_TASK_VIEWS_MONO).state != XRT_CONV_STATE_READY);
		CHECK(fx.create_task(1, XRT_CONV_TASK_VIEWS_MONO, &id, 0, IPC_SERVER_CONV_OUTPUT_AUX_DEPTH) ==
		      XRT_SUCCESS);
		// FAILED (any reason) is terminal for new creates.
		CHECK(fx.create_task(1, XRT_CONV_TASK_DEPTH_VIDEO, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		const xrt_conv_wire_task_desc dd = fx.task(1, XRT_CONV_TASK_DEPTH_VIDEO);
		CHECK(dd.state == XRT_CONV_STATE_FAILED);
		CHECK(dd.failure_reason == XRT_CONV_FAIL_MODEL);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v4 events: opt-in per connection; a v3-only connection never gets one", "[ipc_conv_v4]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->warm_ms = 30;
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		// Connection 7 is v3-only (properties, hint): never latched.
		(void)fx.properties();
		ipc_server_conv_hint_interest(fx.sc, 7, 0);
		CHECK_FALSE(ipc_server_conv_v4_latched(fx.sc, 7));
		// Connection 8 makes a spec-4 call.
		const uint32_t base = fx.task(8, XRT_CONV_TASK_VIEWS_MONO).state;
		CHECK(ipc_server_conv_v4_latched(fx.sc, 8));
		CHECK(base == XRT_CONV_STATE_INSTALLED);

		REQUIRE(ipc_server_conv_v4_warm(fx.sc, 8, XRT_CONV_TASK_VIEWS_MONO) == XRT_SUCCESS);
		std::vector<xrt_conv_wire_event> got;
		REQUIRE(wait_until(
		    [&] {
			    auto e = fx.drain(8);
			    got.insert(got.end(), e.begin(), e.end());
			    return !got.empty() && got.back().state == XRT_CONV_STATE_READY;
		    },
		    3000));
		check_chain(got, XRT_CONV_TASK_VIEWS_MONO, base);
		// The v3 connection saw the same transitions and got nothing.
		CHECK(fx.drain(7).empty());
		CHECK_FALSE(ipc_server_conv_v4_latched(fx.sc, 7));
		// Teardown forgets the latch.
		ipc_server_conv_client_gone(fx.sc, 8);
		CHECK_FALSE(ipc_server_conv_v4_latched(fx.sc, 8));
	}
	conv_fake_fini(&f);
}

TEST_CASE("v4 events: ordered and deduplicated on the EFFECTIVE state", "[ipc_conv_v4]")
{
	conv_fake_provider sel, other;
	conv_fake_init(&sel, "selected");
	conv_fake_init(&other, "other");
	conv_fake_cap *a =
	    conv_fake_add_cap(&sel, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	a->warm_ms = 40;
	conv_fake_add_cap(&other, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(sel, 10) >= 0);    // lower probe order: selected
		REQUIRE(fx.add(other, 200) >= 0); // a fallback behind it
		const uint32_t base = fx.task(5, XRT_CONV_TASK_VIEWS_MONO).state;
		REQUIRE(ipc_server_conv_v4_warm(fx.sc, 5, XRT_CONV_TASK_VIEWS_MONO) == XRT_SUCCESS);
		std::vector<xrt_conv_wire_event> got;
		REQUIRE(wait_until(
		    [&] {
			    auto e = fx.drain(5);
			    got.insert(got.end(), e.begin(), e.end());
			    return fx.task(5, XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY && !got.empty() &&
			           got.back().state == XRT_CONV_STATE_READY;
		    },
		    3000));
		check_chain(got, XRT_CONV_TASK_VIEWS_MONO, base);
		for (size_t i = 1; i < got.size(); i++) {
			const bool dup = got[i].kind == got[i - 1].kind && got[i].task == got[i - 1].task &&
			                 got[i].state == got[i - 1].state &&
			                 got[i].previous_state == got[i - 1].previous_state;
			CHECK_FALSE(dup);
		}

		// The non-selected provider changes state: its row moves, the task's
		// effective state does not, so the app hears nothing.
		conv_fake_set_state(&other, 4, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LOAD, true);
		sleep_ms(300); // several control ticks: the provider-state poll picks it up
		for (const auto &e : fx.drain(5)) {
			CHECK_FALSE(e.kind == XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED);
		}
		CHECK(fx.task(5, XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY);
	}
	conv_fake_fini(&sel);
	conv_fake_fini(&other);
}

TEST_CASE("v4 events: ProvidersChanged on an install, UNAVAILABLE -> READY without a restart", "[ipc_conv_v4]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->state = XRT_CONV_STATE_ABSENT; // its model is not installed yet
	v->warm_ms = 30;
	{
		Fixture fx(CONV_PREWARM_INTEREST);
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		REQUIRE(conv_core_discovery_settled(fx.core));
		CHECK(fx.task(3, XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_ABSENT);
		CHECK(fx.properties().state == XRT_DP_LIFT_STATE_UNAVAILABLE);
		uint64_t id = 0;
		CHECK(fx.create_task(3, XRT_CONV_TASK_VIEWS_MONO, &id) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
		CHECK(fx.drain(3).empty());

		// The installer lands the model; the registry watch says provider_changed.
		conv_fake_set_state(&f, 4, XRT_CONV_STATE_INSTALLED, 0, true);
		conv_core_provider_changed(fx.core, "fake");
		std::vector<xrt_conv_wire_event> got;
		REQUIRE(wait_until(
		    [&] {
			    auto e = fx.drain(3);
			    got.insert(got.end(), e.begin(), e.end());
			    for (const auto &x : got) {
				    if (x.kind == XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED) {
					    return true;
				    }
			    }
			    return false;
		    },
		    3000));
		// The app re-enumerates, warms, and gets READY: no restart anywhere.
		CHECK(fx.task(3, XRT_CONV_TASK_VIEWS_MONO).state != XRT_CONV_STATE_ABSENT);
		REQUIRE(ipc_server_conv_v4_warm(fx.sc, 3, XRT_CONV_TASK_VIEWS_MONO) == XRT_SUCCESS);
		REQUIRE(wait_until(
		    [&] {
			    auto e = fx.drain(3);
			    got.insert(got.end(), e.begin(), e.end());
			    return fx.task(3, XRT_CONV_TASK_VIEWS_MONO).state == XRT_CONV_STATE_READY;
		    },
		    3000));
		check_chain(got, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_STATE_ABSENT);
		bool saw_ready = false;
		REQUIRE(wait_until(
		    [&] {
			    auto e = fx.drain(3);
			    got.insert(got.end(), e.begin(), e.end());
			    for (const auto &x : got) {
				    saw_ready = saw_ready || (x.kind == XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED &&
				                              x.state == XRT_CONV_STATE_READY);
			    }
			    return saw_ready;
		    },
		    1000));
		CHECK(fx.properties().state == XRT_DP_LIFT_STATE_READY);
		CHECK(fx.create_task(3, XRT_CONV_TASK_VIEWS_MONO, &id) == XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v4 stream: served by the v3 per-stream calls; reset marks a discontinuity", "[ipc_conv_v4]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, XRT_CONV_CAP_TEMPORAL);
	v->convert_ms = 1;
	conv_fake_set_state(&f, 4, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		uint64_t id = 0;
		REQUIRE(fx.create_task(4, XRT_CONV_TASK_VIEWS_MONO, &id, 4, IPC_SERVER_CONV_OUTPUT_AUX_DEPTH) ==
		        XRT_SUCCESS);
		bool ready = false, disc = false;
		ipc_lift_result r3{};
		xrt_conv_wire_result r4{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(4, id);
			    return ipc_server_conv_v4_acquire(fx.sc, 4, id, &ready, &disc, &r3, &r4) == XRT_SUCCESS &&
			           ready;
		    },
		    3000));
		CHECK(r3.view_count == 4u);
		CHECK(r4.views_info.view_count == 4u);
		CHECK((r4.written & XRT_CONV_OUT_DEPTH) != 0u);
		CHECK(r4.depth_info.kind == XRT_CONV_DEPTH_METRIC);
		CHECK(r4.frame_id == r3.frame_id);

		// Let the frames in flight land first: the acquire is latest-wins, so
		// the next result to read must be the one after the reset.
		REQUIRE(wait_until(
		    [&] {
			    ready = true;
			    while (ready) {
				    REQUIRE(ipc_server_conv_v4_acquire(fx.sc, 4, id, &ready, &disc, &r3, &r4) ==
				            XRT_SUCCESS);
			    }
			    sleep_ms(30);
			    REQUIRE(ipc_server_conv_v4_acquire(fx.sc, 4, id, &ready, &disc, &r3, &r4) == XRT_SUCCESS);
			    return !ready;
		    },
		    3000));
		REQUIRE(ipc_server_conv_v4_stream_reset(fx.sc, 4, id) == XRT_SUCCESS);
		CHECK(ipc_server_conv_v4_stream_reset(fx.sc, 5, id) ==
		      XRT_ERROR_OUTPUT_REQUEST_FAILURE); // not its stream
		REQUIRE(fx.submit(4, id) == XRT_SUCCESS);
		REQUIRE(wait_until(
		    [&] {
			    return ipc_server_conv_v4_acquire(fx.sc, 4, id, &ready, &disc, &r3, &r4) == XRT_SUCCESS &&
			           ready;
		    },
		    3000));
		CHECK(disc); // the first result after the reset says so
		REQUIRE(fx.submit(4, id) == XRT_SUCCESS);
		REQUIRE(wait_until(
		    [&] {
			    return ipc_server_conv_v4_acquire(fx.sc, 4, id, &ready, &disc, &r3, &r4) == XRT_SUCCESS &&
			           ready;
		    },
		    3000));
		CHECK_FALSE(disc); // per frame, not sticky
		os_mutex_lock(&f.mtx);
		CHECK(f.resets_seen >= 1u);
		os_mutex_unlock(&f.mtx);
	}
	conv_fake_fini(&f);
}

TEST_CASE("depth-kind contract: task depthKinds, the v3 reading, XrLiftDepthInfoDXR", "[ipc_conv_v4]")
{
	// Every kind the ABI defines maps to exactly one app bit and one v3 reading.
	struct Row
	{
		uint32_t kind;
		XrLiftDepthSemanticsDXR units;
		XrLiftDepthEncodingDXR enc;
		bool metric;
	};
	const Row rows[] = {
	    {XRT_CONV_DEPTH_METRIC, XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR, XR_LIFT_DEPTH_ENCODING_LINEAR_DXR, true},
	    {XRT_CONV_DEPTH_METRIC_INVERSE, XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR, XR_LIFT_DEPTH_ENCODING_INVERSE_DXR,
	     true},
	    {XRT_CONV_DEPTH_AFFINE_INVERSE, XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR, XR_LIFT_DEPTH_ENCODING_INVERSE_DXR,
	     false},
	    {XRT_CONV_DEPTH_AFFINE_DISPARITY, XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR, XR_LIFT_DEPTH_ENCODING_INVERSE_DXR,
	     false},
	    {XRT_CONV_DEPTH_AFFINE, XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR, XR_LIFT_DEPTH_ENCODING_LINEAR_DXR, false},
	};
	for (const Row &row : rows) {
		xrt_conv_wire_task_desc d{};
		d.task = XRT_CONV_TASK_VIEWS_MONO;
		d.outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
		d.depth_kind = row.kind;
		d.lanes = XRT_CONV_LANE_STREAM;
		XrLiftTaskPropertiesDXR p{XR_TYPE_LIFT_TASK_PROPERTIES_DXR};
		oxr_conv_task_properties_from_wire(&d, 0, 0, &p);
		CHECK(p.depthKinds == (1ull << row.kind));
		CHECK(((p.outputs & XR_LIFT_OUTPUT_METRIC_BIT_DXR) != 0) == row.metric);
		CHECK((p.outputs & XR_LIFT_OUTPUT_AUX_DEPTH_BIT_DXR) != 0); // auxDepthSupported stays true
		XrLiftDepthSemanticsDXR u{};
		XrLiftDepthEncodingDXR e{};
		REQUIRE(oxr_conv_depth_kind_to_v3(row.kind, &u, &e));
		CHECK(u == row.units);
		CHECK(e == row.enc);
	}
	// PIXEL_DISPARITY never reaches a v3 aux result.
	XrLiftDepthSemanticsDXR u{};
	XrLiftDepthEncodingDXR e{};
	CHECK_FALSE(oxr_conv_depth_kind_to_v3(XRT_CONV_DEPTH_STEREO_DISPARITY_PX, &u, &e));

	// Units are never invented: affine depth without intrinsics reads UNKNOWN,
	// with no focal length, whatever source the provider claimed.
	xrt_conv_wire_depth_info w{};
	w.kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
	w.intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
	w.value_scale = 0.0f; // read as 1
	w.invalid_sample = std::nanf("");
	w.temporal = 1;
	XrLiftDepthInfoDXR o{XR_TYPE_LIFT_DEPTH_INFO_DXR};
	oxr_conv_depth_info_from_wire(&w, true, &o);
	CHECK(o.kind == XR_LIFT_DEPTH_KIND_AFFINE_DISPARITY_DXR);
	CHECK(o.rawScale == 1.0f);
	CHECK(o.invalidValueValid == XR_FALSE);
	CHECK(o.intrinsicsSource == XR_LIFT_INTRINSICS_SOURCE_UNKNOWN_DXR);
	CHECK(o.focalLengthPx.x == 0.0f);
	CHECK(o.nearZ == 0.0f);
	CHECK(o.farZ == 0.0f);
	CHECK(o.temporallyConsistent == XR_TRUE);
	CHECK(o.discontinuity == XR_TRUE);
	CHECK(o.type == XR_TYPE_LIFT_DEPTH_INFO_DXR);
	// Metric depth with real intrinsics keeps them and their source.
	w.kind = XRT_CONV_DEPTH_METRIC;
	w.intrinsics_source = XRT_CONV_INTRINSICS_ESTIMATED;
	w.fx = 500.0f;
	w.fy = 501.0f;
	w.cx = 320.0f;
	w.cy = 240.0f;
	w.invalid_sample = -1.0f;
	w.near_value = 0.5f;
	w.far_value = 20.0f;
	oxr_conv_depth_info_from_wire(&w, false, &o);
	CHECK(o.kind == XR_LIFT_DEPTH_KIND_METRIC_DEPTH_DXR);
	CHECK(o.intrinsicsSource == XR_LIFT_INTRINSICS_SOURCE_ESTIMATED_DXR);
	CHECK(o.focalLengthPx.y == 501.0f);
	CHECK(o.convention == XR_LIFT_COORDINATE_SYSTEM_OPENCV_RDF_DXR);
	CHECK(o.invalidValueValid == XR_TRUE);
	CHECK(o.invalidValue == -1.0f);
	CHECK(o.nearZ == 0.5f);
	CHECK(o.farZ == 20.0f);
	CHECK(o.discontinuity == XR_FALSE);
}
