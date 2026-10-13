// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The four changes a spec-3 XR_DXR_lift caller can observe now that
 *         lift runs on conversion providers (ADR-053; spec
 *         docs/specs/extensions/XR_DXR_lift.md §14.2), one golden test each,
 *         at the v3 adapter the lift_* handlers call (#2054).
 *
 * Everything else a spec-3 caller sees is pinned unchanged by tests_lift_abi
 * (layouts, values, the function table) and tests_oxr_lift_startup (the
 * startup sequence on the wire).
 */

#include "ipc_conv_v4_fixture.hpp"

TEST_CASE("v3 shift 1: maxStreams is the selected provider's per-task limit", "[lift_v3_shift]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->cap.max_streams = 3;
	conv_fake_cap *d =
	    conv_fake_add_cap(&f, 2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_DEPTH, 0);
	d->cap.max_streams = 5;
	conv_fake_set_state(&f, 4, XRT_CONV_STATE_READY, 0, false);
	conv_fake_set_state(&f, 2, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		CHECK(fx.properties().max_streams == 3u); // VIEWS_MONO's, not a runtime-wide number
		uint64_t ids[4] = {};
		for (int i = 0; i < 3; i++) {
			REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0,
			                                           &ids[i]) == XRT_SUCCESS);
		}
		CHECK(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &ids[3]) ==
		      XRT_ERROR_CLIENT_LIMIT_REACHED);
		// The DEPTH task has its own budget: the views streams do not use it up.
		uint64_t did = 0;
		CHECK(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_DEPTH, 0, 1.0f, 0, 0, &did) ==
		      XRT_SUCCESS);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v3 shift 2: GAUSSIANS may return SOG with the camera block", "[lift_v3_shift]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *s =
	    conv_fake_add_cap(&f, 6, XRT_CONV_TASK_SPLATS_MONO, XRT_CONV_LANE_JOB, XRT_CONV_OUT_SPLAT_BLOB, 0);
	s->cap.splat_formats = 1u << XRT_CONV_BLOB_SOG;
	s->job_ms = 10;
	s->job_bytes = 4096;
	s->job_blob_format = XRT_CONV_BLOB_SOG;
	conv_fake_set_state(&f, 6, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx;
		fx.install_glue(/*cpu*/ true);
		REQUIRE(fx.add(f) >= 0);
		CHECK((fx.properties().modes & XRT_DP_LIFT_MODE_GAUSSIANS) != 0u);
		REQUIRE(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_GAUSSIANS, XRT_CONV_CONTENT_PHOTO));
		uint64_t id = 0;
		REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_GAUSSIANS, XRT_CONV_CONTENT_PHOTO,
		                                           1.0f, 0, 0, &id) == XRT_SUCCESS);
		REQUIRE(fx.submit(1, id) == XRT_SUCCESS);
		bool ready = false;
		xrt_lift_blob_info info{};
		uint8_t *bytes = nullptr;
		REQUIRE(wait_until(
		    [&] {
			    return ipc_server_conv_lift_acquire_blob(fx.sc, 1, id, 1u << 20, &ready, &info, &bytes) ==
			               XRT_SUCCESS &&
			           ready;
		    },
		    3000));
		CHECK(info.format == XRT_DP_LIFT_BLOB_SOG);
		CHECK(info.byte_count == 4096u);
		std::free(bytes);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v3 shift 3: AFFINE_DISPARITY aux depth reads RELATIVE + INVERSE", "[lift_v3_shift]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v = conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM,
	                                     XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH, 0);
	v->cap.depth_kind = XRT_CONV_DEPTH_AFFINE_DISPARITY;
	v->convert_ms = 1;
	conv_fake_set_state(&f, 4, XRT_CONV_STATE_READY, 0, false);
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		const xrt_dp_lift_caps p = fx.properties();
		CHECK((p.aux_outputs & XRT_DP_LIFT_AUX_DEPTH) != 0u);
		CHECK(p.aux_depth_semantics == XRT_DP_LIFT_DEPTH_RELATIVE);
		uint64_t id = 0;
		REQUIRE(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f,
		                                           XRT_DP_LIFT_AUX_DEPTH, 0, &id) == XRT_SUCCESS);
		bool ready = false;
		ipc_lift_result r{};
		REQUIRE(wait_until(
		    [&] {
			    fx.submit(1, id);
			    return ipc_server_conv_lift_acquire(fx.sc, 1, id, &ready, &r) == XRT_SUCCESS && ready;
		    },
		    3000));
		CHECK(r.depth.valid == 1u);
		CHECK(r.depth.units == XRT_DP_LIFT_DEPTH_RELATIVE);
		CHECK(r.depth.encoding == XRT_DP_LIFT_DEPTH_ENCODING_INVERSE);
		CHECK(r.depth.intrinsics_valid == 0u); // never a faked scale
		CHECK(r.depth.transform_valid == 0u);
	}
	conv_fake_fini(&f);
}

TEST_CASE("v3 shift 4: a mode backed by a warming task is accepted at create", "[lift_v3_shift]")
{
	conv_fake_provider f;
	conv_fake_init(&f, "fake");
	conv_fake_cap *v =
	    conv_fake_add_cap(&f, 4, XRT_CONV_TASK_VIEWS_MONO, XRT_CONV_LANE_STREAM, XRT_CONV_OUT_VIEWS, 0);
	v->warm_ms = 2000;
	{
		Fixture fx;
		fx.install_glue();
		REQUIRE(fx.add(f) >= 0);
		const xrt_dp_lift_caps p = fx.properties();
		CHECK(p.state == XRT_DP_LIFT_STATE_ACTIVATING);
		CHECK((p.modes & XRT_DP_LIFT_MODE_SBS) != 0u); // advertised while not READY
		CHECK(ipc_server_conv_routes_create(fx.sc, XRT_DP_LIFT_MODE_SBS, 0));
		uint64_t id = 0;
		CHECK(ipc_server_conv_lift_stream_create(fx.sc, 1, XRT_DP_LIFT_MODE_SBS, 0, 1.0f, 0, 0, &id) ==
		      XRT_SUCCESS);
		CHECK(fx.task(1, XRT_CONV_TASK_VIEWS_MONO).state != XRT_CONV_STATE_READY);
	}
	conv_fake_fini(&f);
}
