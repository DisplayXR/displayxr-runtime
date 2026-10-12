// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #2005: the XR_DXR_lift v3 startup sequence, pinned before ADR-053
 *         re-points the handlers at the conversion core.
 *
 * What an app does to get its first lifted frame, and what the runtime answers
 * and puts on the wire for it:
 *
 *   xrGetLiftPropertiesDXR -> xrCreateLiftStreamDXR -> xrSubmitLiftFrameDXR ->
 *   xrAcquireLiftResultDXR (NOT_READY ... ready, first export) -> destroy
 *
 * over both routes to the one conversion module: an IPC session's own
 * connection, and the in-process D3D11 / D3D12 session's lift-only link
 * (ADR-049), including the ACTIVATING and UNAVAILABLE answers a shipping
 * browser polls on.
 *
 * Host-only: no GPU, no service, no window. The REAL oxr_lift.c entry points
 * and the REAL ipc_client_lift.c client are compiled in; the wire calls under
 * them (ipc_call_lift_*), the lift link and the session's connection bridge are
 * faked in tests_oxr_lift_startup_fake.c, which records every call in order.
 * The trace strings below ARE the pinned contract: a refactor that adds,
 * drops, reorders or reshapes a message fails here.
 *
 * xrCreateInstance itself is not driven (it needs a display processor); the
 * fixture hands the entry points an instance with XR_DXR_lift enabled, and one
 * case pins what a lift call answers when it is not.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_dp_lift.h"

#include "tests_oxr_lift_startup_fake.h"

#include "shared/ipc_lift_caps.h"

#include "oxr_api_funcs.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>


namespace {

std::vector<std::string>
take_trace()
{
	std::vector<std::string> out;
	for (size_t i = 0; i < lf_trace_count(); i++) {
		out.emplace_back(lf_trace_line(i));
	}
	lf_trace_clear();
	return out;
}

XrLiftPropertiesDXR
props_init(void *next = nullptr)
{
	XrLiftPropertiesDXR p = {};
	p.type = XR_TYPE_LIFT_PROPERTIES_DXR;
	p.next = next;
	return p;
}

XrLiftStreamCreateInfoDXR
create_info(XrLiftModeDXR mode, const void *next = nullptr)
{
	XrLiftStreamCreateInfoDXR ci = {};
	ci.type = XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR;
	ci.next = next;
	ci.mode = mode;
	ci.contentHint = XR_LIFT_CONTENT_HINT_VIDEO_DXR;
	ci.inputScale = 0.0f; // "0 is read as 1.0"
	return ci;
}

XrLiftFrameSubmitInfoDXR
submit_info(const void *next = nullptr)
{
	XrLiftFrameSubmitInfoDXR si = {};
	si.type = XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR;
	si.next = next;
	si.inputTexture = (void *)(intptr_t)0x1234;
	si.inputIsDxgi = XR_FALSE;
	si.extent = {1920, 1080};
	si.sourceTime = 42;
	return si;
}

XrLiftResultDXR
result_init()
{
	XrLiftResultDXR r = {};
	r.type = XR_TYPE_LIFT_RESULT_DXR;
	return r;
}

using V = std::vector<std::string>;

} // namespace


TEST_CASE("lift startup, IPC session: ACTIVATING -> stream -> submit -> NOT_READY -> READY -> first result")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, true);

	// 1. First properties query of the service's lifetime: the module is
	//    being brought up. The answer is ACTIVATING (poll), never an error.
	lf_set_properties(XRT_DP_LIFT_STATE_ACTIVATING, 0, 0, 0, 0);
	XrLiftPropertiesDXR props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_ACTIVATING_DXR);
	CHECK(props.supportedModes == 0);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_get_properties -> state=1 modes=0x0 max_views=0 aux=0x0",
	                      });

	// 2. Streams may be created while ACTIVATING (spec §2).
	XrLiftStreamCreateInfoDXR ci = create_info(XR_LIFT_MODE_SBS_DXR);
	XrLiftStreamDXR stream = XR_NULL_HANDLE;
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_SUCCESS);
	REQUIRE(stream != XR_NULL_HANDLE);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_stream_create(mode=2, hint=0, scale=1.00, aux=0, aux_fmt=0)",
	                      });

	// 3. First submit, no options: the stream's defaults; one input handle.
	XrLiftFrameSubmitInfoDXR si = submit_info();
	uint64_t frame_id = 0;
	REQUIRE(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_SUCCESS);
	CHECK(frame_id == 1);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_submit_frame(id=0x51, 1920x1080, t=42, has_params=0, view_count=0, "
	                          "viewpoints=0, handles=1)",
	                      });

	// 4. Poll before the module has produced anything: NOT_READY (a success
	//    code), no export calls.
	XrLiftResultDXR res = result_init();
	CHECK(oxr_xrAcquireLiftResultDXR(stream, &res) == XR_LIFT_NOT_READY_DXR);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_acquire_result(id=0x51) -> ready=0 realloc=0",
	                      });

	// 5. The app re-polls properties (<= 2 Hz) and sees READY.
	lf_set_properties(XRT_DP_LIFT_STATE_READY, XRT_DP_LIFT_MODE_DEPTH | XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW,
	                  8, 0, 0);
	props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_READY_DXR);
	CHECK(props.supportedModes == (XR_LIFT_MODE_DEPTH_BIT_DXR | XR_LIFT_MODE_SBS_BIT_DXR | XR_LIFT_MODE_NVIEW_BIT_DXR));
	CHECK(props.maxStreams == 4);
	CHECK(props.maxViews == 8);
	CHECK(props.depthSemantics == XR_LIFT_DEPTH_SEMANTICS_RELATIVE_DXR);
	CHECK(std::string(props.backend) == "fake-module");
	CHECK(props.typicalLatency == 30000000);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_get_properties -> state=2 modes=0x7 max_views=8 aux=0x0",
	                      });

	// 6. First result: the acquire, then the one-time export of the output
	//    texture and its fence (handles on first acquire / reallocation).
	lf_set_output_realloc_next(true);
	lf_set_result_ready(true, 3840, 1080, 2);
	res = result_init();
	REQUIRE(oxr_xrAcquireLiftResultDXR(stream, &res) == XR_SUCCESS);
	CHECK(res.frameId == 1);
	CHECK(res.sourceTime == 777);
	CHECK(res.fenceValue == 9);
	CHECK(res.extent.width == 3840);
	CHECK(res.extent.height == 1080);
	CHECK(res.viewCount == 2);
	CHECK(res.latency == 25000000);
	CHECK(res.outputTexture == (void *)(intptr_t)LF_OUTPUT_HANDLE);
	CHECK(res.fence == (void *)(intptr_t)LF_FENCE_HANDLE);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_acquire_result(id=0x51) -> ready=1 realloc=1",
	                          "wire:lift_get_output(id=0x51, max_handles=1)",
	                          "wire:lift_get_fence(id=0x51, max_handles=1)",
	                      });

	// 7. Steady state: the export is latched, no handles, no export calls.
	lf_set_result_ready(true, 3840, 1080, 2);
	res = result_init();
	REQUIRE(oxr_xrAcquireLiftResultDXR(stream, &res) == XR_SUCCESS);
	CHECK(res.outputTexture == nullptr);
	CHECK(res.fence == nullptr);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_acquire_result(id=0x51) -> ready=1 realloc=0",
	                      });

	// 8. Destroying the stream tells the service.
	REQUIRE(oxr_xrDestroyLiftStreamDXR(stream) == XR_SUCCESS);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_stream_destroy(id=0x51)",
	                      });

	lf_session_destroy();
	CHECK(take_trace().empty());
}

TEST_CASE("lift startup, IPC session: UNAVAILABLE service")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, true);

	// No module (macOS / Linux service, a plug-in without lift, DXR_LIFT=0):
	// success, modes 0, UNAVAILABLE -- the open default, never an error.
	lf_set_properties(XRT_DP_LIFT_STATE_UNAVAILABLE, 0, 0, 0, 0);
	XrLiftPropertiesDXR props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_UNAVAILABLE_DXR);
	CHECK(props.supportedModes == 0);
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_get_properties -> state=0 modes=0x0 max_views=0 aux=0x0",
	                      });

	// The service refuses the stream: FEATURE_UNSUPPORTED, session usable.
	lf_set_create_xret(XRT_ERROR_FEATURE_NOT_SUPPORTED);
	XrLiftStreamCreateInfoDXR ci = create_info(XR_LIFT_MODE_SBS_DXR);
	XrLiftStreamDXR stream = XR_NULL_HANDLE;
	CHECK(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(stream == XR_NULL_HANDLE);
	CHECK_FALSE(lf_session_lost());
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_stream_create(mode=2, hint=0, scale=1.00, aux=0, aux_fmt=0)",
	                      });

	// maxStreams reached maps to LIMIT_REACHED.
	lf_set_create_xret(XRT_ERROR_CLIENT_LIMIT_REACHED);
	CHECK(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_ERROR_LIMIT_REACHED);
	take_trace();

	lf_session_destroy();
}

TEST_CASE("lift startup, IPC session: a dead pipe loses the instance, then the session")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, true);

	lf_set_properties_xret(XRT_ERROR_IPC_FAILURE);
	XrLiftPropertiesDXR props = props_init();
	CHECK(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_ERROR_INSTANCE_LOST);
	CHECK(lf_session_lost());
	CHECK(take_trace() == V{
	                          "bridge:session_connection",
	                          "wire:lift_get_properties -> state=0 modes=0x0 max_views=0 aux=0x0",
	                      });

	// Every later call: SESSION_LOST, nothing on the wire.
	props = props_init();
	CHECK(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_ERROR_SESSION_LOST);
	CHECK(take_trace().empty());

	lf_session_destroy();
}

TEST_CASE("lift startup: XR_DXR_lift not enabled, or an ineligible in-process session")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, /* lift_enabled */ false);
	XrLiftPropertiesDXR props = props_init();
	CHECK(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_ERROR_FUNCTION_UNSUPPORTED);
	CHECK(take_trace().empty());
	lf_session_destroy();

	// An in-process GL session has no route to the module (ADR-049 covers
	// D3D11 / D3D12 on Windows only): FEATURE_UNSUPPORTED, nothing opened.
	lf_reset();
	session = lf_session_create(LF_SESSION_INPROCESS_GL, true);
	props = props_init();
	CHECK(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_ERROR_FEATURE_UNSUPPORTED);
	XrLiftStreamCreateInfoDXR ci = create_info(XR_LIFT_MODE_SBS_DXR);
	XrLiftStreamDXR stream = XR_NULL_HANDLE;
	CHECK(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(take_trace().empty());
	lf_session_destroy();
}

#ifdef XRT_OS_WINDOWS
TEST_CASE("lift startup, in-process D3D12 session over the ADR-049 lift-only link")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_INPROCESS_D3D12, true);

	// 1. First query: the link is created lazily and its background connect
	//    is in flight. ACTIVATING, with nothing on the wire.
	lf_set_link(0 /* CONNECTING */, 0);
	XrLiftPropertiesDXR props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_ACTIVATING_DXR);
	CHECK(props.supportedModes == 0);
	CHECK(take_trace() == V{
	                          "link:create",
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTING gen=0",
	                      });

	// 2. Creating while the connect is still in flight: a bounded wait (setup
	//    call), then a retryable RUNTIME_FAILURE.
	XrLiftStreamCreateInfoDXR ci = create_info(XR_LIFT_MODE_SBS_DXR);
	XrLiftStreamDXR stream = XR_NULL_HANDLE;
	CHECK(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_ERROR_RUNTIME_FAILURE);
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=6000) -> CONNECTING gen=0",
	                      });

	// 3. No service: UNAVAILABLE (open default); creating is FEATURE_UNSUPPORTED.
	lf_set_link(2 /* FAILED */, 0);
	props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_UNAVAILABLE_DXR);
	CHECK(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> FAILED gen=0",
	                          "link:acquire_wait(timeout_ms=6000) -> FAILED gen=0",
	                      });

	// 4. Connected (generation 1): the module answers over the link; every
	//    call hands the connection back.
	lf_set_link(1 /* CONNECTED */, 1);
	lf_set_properties(XRT_DP_LIFT_STATE_READY, XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW, 8, 0, 0);
	props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_READY_DXR);
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_SUCCESS);
	XrLiftFrameSubmitInfoDXR si = submit_info();
	uint64_t frame_id = 0;
	REQUIRE(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_SUCCESS);
	lf_set_output_realloc_next(true);
	lf_set_result_ready(true, 3840, 1080, 2);
	XrLiftResultDXR res = result_init();
	REQUIRE(oxr_xrAcquireLiftResultDXR(stream, &res) == XR_SUCCESS);
	CHECK(res.outputTexture == (void *)(intptr_t)LF_OUTPUT_HANDLE);
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=1",
	                          "wire:lift_get_properties -> state=2 modes=0x6 max_views=8 aux=0x0",
	                          "link:release(gen=1, ipc_failed=0)",
	                          "link:acquire_wait(timeout_ms=6000) -> CONNECTED gen=1",
	                          "wire:lift_stream_create(mode=2, hint=0, scale=1.00, aux=0, aux_fmt=0)",
	                          "link:release(gen=1, ipc_failed=0)",
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=1",
	                          "wire:lift_submit_frame(id=0x51, 1920x1080, t=42, has_params=0, view_count=0, "
	                          "viewpoints=0, handles=1)",
	                          "link:release(gen=1, ipc_failed=0)",
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=1",
	                          "wire:lift_acquire_result(id=0x51) -> ready=1 realloc=1",
	                          "wire:lift_get_output(id=0x51, max_handles=1)",
	                          "wire:lift_get_fence(id=0x51, max_handles=1)",
	                          "link:release(gen=1, ipc_failed=0)",
	                      });

	// 5. The link reconnected (generation 2): the old stream died with its
	//    connection -- FEATURE_UNSUPPORTED, nothing sent.
	lf_set_link(1 /* CONNECTED */, 2);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_ERROR_FEATURE_UNSUPPORTED);
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=2",
	                          "link:release(gen=2, ipc_failed=0)",
	                      });
	REQUIRE(oxr_xrDestroyLiftStreamDXR(stream) == XR_SUCCESS);
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=2",
	                          "link:release(gen=2, ipc_failed=0)",
	                      });

	// 6. A dead lift pipe never loses an in-process session: the call is a
	//    transient RUNTIME_FAILURE and the link is marked dead.
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_SUCCESS);
	take_trace();
	lf_set_submit_xret(XRT_ERROR_IPC_FAILURE);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_ERROR_RUNTIME_FAILURE);
	CHECK_FALSE(lf_session_lost());
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> CONNECTED gen=2",
	                          "wire:lift_submit_frame(id=0x52, 1920x1080, t=42, has_params=0, view_count=0, "
	                          "viewpoints=0, handles=1)",
	                          "link:release(gen=2, ipc_failed=1)",
	                      });
	props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.state == XR_LIFT_STATE_UNAVAILABLE_DXR);
	take_trace();

	// Teardown: the stream (its connection is gone: nothing sent), then the
	// session's link.
	lf_session_destroy();
	CHECK(take_trace() == V{
	                          "link:acquire_wait(timeout_ms=0) -> DEAD gen=2",
	                          "link:destroy",
	                      });
}
#endif // XRT_OS_WINDOWS


/*
 *
 * #2005 G1 -- the aux-depth caps over IPC.
 *
 */

TEST_CASE("lift caps: every field round-trips through the wire copy (G1)")
{
	// Tripwire: a field appended to either struct must be added to
	// ipc_lift_caps.h (and to this test) -- the hand copy is how G1 happened.
	STATIC_REQUIRE(sizeof(struct xrt_dp_lift_caps) == 224);
	STATIC_REQUIRE(sizeof(struct ipc_lift_properties) == 224);

	struct xrt_dp_lift_caps in;
	uint8_t *p = (uint8_t *)&in;
	for (size_t i = 0; i < sizeof(in); i++) {
		p[i] = (uint8_t)(i * 7 + 3) | 1u; // never 0 inside a string
	}
	in.struct_size = (uint32_t)sizeof(in);
	in.backend[sizeof(in.backend) - 1] = '\0';
	in.module_version[sizeof(in.module_version) - 1] = '\0';
	in.module_detail[sizeof(in.module_detail) - 1] = '\0';

	struct ipc_lift_properties wire;
	std::memset(&wire, 0xAB, sizeof(wire));
	ipc_lift_properties_from_caps(&in, &wire);
	CHECK(wire.reserved == 0);
	CHECK(wire.reserved2 == 0);

	struct xrt_dp_lift_caps out;
	std::memset(&out, 0xCD, sizeof(out));
	ipc_lift_caps_from_properties(&wire, &out);

	CHECK(out.struct_size == sizeof(out));
	CHECK(out.modes == in.modes);
	CHECK(out.max_streams == in.max_streams);
	CHECK(out.max_views == in.max_views);
	CHECK(out.depth_semantics == in.depth_semantics);
	CHECK(out.state == in.state);
	CHECK(out.typical_latency_ns == in.typical_latency_ns);
	CHECK(std::string(out.backend) == std::string(in.backend));
	CHECK(out.aux_outputs == in.aux_outputs);                 // G1
	CHECK(out.aux_depth_semantics == in.aux_depth_semantics); // G1
	CHECK(out.module_status == in.module_status);
	CHECK(std::string(out.module_version) == std::string(in.module_version));
	CHECK(std::string(out.module_detail) == std::string(in.module_detail));

	// And byte for byte, up to the end of the last field (tail padding aside).
	const size_t end = offsetof(struct xrt_dp_lift_caps, module_detail) + sizeof(in.module_detail);
	CHECK(std::memcmp(&in, &out, end) == 0);
}

TEST_CASE("lift caps: auxDepthSupported reaches the app over IPC (G1)")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, true);
	lf_set_properties(XRT_DP_LIFT_STATE_READY, XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW, 8,
	                  XRT_DP_LIFT_AUX_DEPTH, XRT_DP_LIFT_DEPTH_METRIC);

	XrLiftDepthPropertiesDXR dp = {};
	dp.type = XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR;
	XrLiftPropertiesDXR props = props_init(&dp);
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(dp.auxDepthSupported == XR_TRUE);
	CHECK(dp.auxDepthSemantics == XR_LIFT_DEPTH_SEMANTICS_METRIC_DXR);

	// A module without aux depth still reads false.
	lf_set_properties(XRT_DP_LIFT_STATE_READY, XRT_DP_LIFT_MODE_SBS, 8, 0, 0);
	dp = {};
	dp.type = XR_TYPE_LIFT_DEPTH_PROPERTIES_DXR;
	props = props_init(&dp);
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(dp.auxDepthSupported == XR_FALSE);
	take_trace();
	lf_session_destroy();
}


/*
 *
 * #2005 G2 -- maxViews enforced.
 *
 */

#include "oxr_lift_rules.h"

namespace {

XrLiftOptionsDXR
options(uint32_t view_count)
{
	XrLiftOptionsDXR o = {};
	o.type = XR_TYPE_LIFT_OPTIONS_DXR;
	o.convergence = -1.0f;
	o.strength = 1.0f;
	o.inpaint = XR_TRUE;
	o.viewpointSource = XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR;
	o.viewCount = view_count;
	return o;
}

} // namespace

TEST_CASE("lift view rule: the module's maxViews bounds NVIEW (G2)")
{
	// Unknown (0) or over-reported bounds fall back to the constant ceiling.
	CHECK(oxr_lift_view_bound(0) == XRT_LIFT_MAX_VIEWS);
	CHECK(oxr_lift_view_bound(4) == 4);
	CHECK(oxr_lift_view_bound(XRT_LIFT_MAX_VIEWS + 5) == XRT_LIFT_MAX_VIEWS);

	CHECK(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_NVIEW, 4, 4));
	CHECK_FALSE(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_NVIEW, 5, 4));
	CHECK(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_NVIEW, 0, 4)); // 0 = module default
	CHECK(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_NVIEW, XRT_LIFT_MAX_VIEWS, 0));
	CHECK_FALSE(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_NVIEW, XRT_LIFT_MAX_VIEWS + 1, 0));
	// maxViews is the NVIEW bound (spec §2); the constant ceiling is everyone's.
	CHECK(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_SBS, 6, 4));
	CHECK_FALSE(oxr_lift_view_count_ok(XRT_DP_LIFT_MODE_SBS, XRT_LIFT_MAX_VIEWS + 1, 4));
}

TEST_CASE("lift submit: viewCount past the module's maxViews is VALIDATION_FAILURE, nothing sent (G2)")
{
	lf_reset();
	XrSession session = lf_session_create(LF_SESSION_IPC, true);

	// A stream created while ACTIVATING (bound not known yet) ...
	lf_set_properties(XRT_DP_LIFT_STATE_ACTIVATING, 0, 0, 0, 0);
	XrLiftPropertiesDXR props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	XrLiftStreamCreateInfoDXR ci = create_info(XR_LIFT_MODE_NVIEW_DXR);
	XrLiftStreamDXR stream = XR_NULL_HANDLE;
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &ci, &stream) == XR_SUCCESS);
	take_trace();

	// ... is held to the constant ceiling only.
	XrLiftOptionsDXR o = options(8);
	XrLiftFrameSubmitInfoDXR si = submit_info(&o);
	uint64_t frame_id = 0;
	CHECK(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_SUCCESS);
	CHECK(lf_last_submit_view_count() == 8);
	take_trace();

	// The module comes up READY with maxViews 4: the same stream is now held
	// to 4.
	lf_set_properties(XRT_DP_LIFT_STATE_READY, XRT_DP_LIFT_MODE_SBS | XRT_DP_LIFT_MODE_NVIEW, 4, 0, 0);
	props = props_init();
	REQUIRE(oxr_xrGetLiftPropertiesDXR(session, &props) == XR_SUCCESS);
	CHECK(props.maxViews == 4);
	take_trace();

	o = options(6);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_ERROR_VALIDATION_FAILURE);
	CHECK(take_trace().empty()); // caller bug, nothing sent
	CHECK_FALSE(lf_session_lost());

	o = options(4);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream, &si, &frame_id) == XR_SUCCESS);
	CHECK(lf_last_submit_view_count() == 4);
	take_trace();

	// A stream created after READY latches the bound at create.
	XrLiftStreamDXR stream2 = XR_NULL_HANDLE;
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &ci, &stream2) == XR_SUCCESS);
	o = options(5);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream2, &si, &frame_id) == XR_ERROR_VALIDATION_FAILURE);

	// SBS is not bounded by maxViews (only by the constant ceiling).
	XrLiftStreamCreateInfoDXR sbs = create_info(XR_LIFT_MODE_SBS_DXR);
	XrLiftStreamDXR stream3 = XR_NULL_HANDLE;
	REQUIRE(oxr_xrCreateLiftStreamDXR(session, &sbs, &stream3) == XR_SUCCESS);
	o = options(6);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream3, &si, &frame_id) == XR_SUCCESS);
	o = options(9);
	CHECK(oxr_xrSubmitLiftFrameDXR(stream3, &si, &frame_id) == XR_ERROR_VALIDATION_FAILURE);

	take_trace();
	lf_session_destroy();
}
