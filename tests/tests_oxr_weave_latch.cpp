// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1427: the XR_DXR_weave handle-export latch must not arm on a miss.
 *
 * `xrWeaveSubmitDXR` hands the woven texture/fence HANDLEs to the caller once
 * (spec §2: "on the first successful submit and on reallocation") and latches
 * that it did. It used to latch even when the export produced nothing — a
 * Windows output that did not exist yet, a refused peer handle duplication, or
 * the Android #1277 weave satellite reporting "no output" BY DESIGN — after
 * which `weavedTexture` was NULL forever at those dims and the caller drew black
 * tiles for the life of the session.
 *
 * Driving the real entry point needs a service, a present-owner IPC session and
 * a bound window, none of which exist on a host CI runner, and the two export
 * bridges are link-time symbols rather than vtable slots, so they cannot be
 * faked from a test. The *decision* is therefore a pure boolean in
 * oxr_weave_latch.h and that is what is pinned here — the same "make the rule
 * host-testable" shape as the ADR-027 tier-1 dispatch and the mini-window tell.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_config_os.h"

#include "oxr_weave_latch.h"

TEST_CASE("weave export latch: never arms without a texture")
{
	// The #1427 regression itself: a success-with-no-handle frame (Windows
	// before weave_output_handle exists, Android while the satellite is live)
	// must leave the session asking again next frame.
	CHECK_FALSE(oxr_weave_should_latch_export(false, /* tex */ false, /* fence */ false));
	CHECK_FALSE(oxr_weave_should_latch_export(true, /* tex */ false, /* fence */ false));

	// Not even a fence on its own rescues it — the texture is the payload.
	CHECK_FALSE(oxr_weave_should_latch_export(true, /* tex */ false, /* fence */ true));
	CHECK_FALSE(oxr_weave_should_latch_export(false, /* tex */ false, /* fence */ true));
}

TEST_CASE("weave export latch: POSIX latches on the texture alone")
{
	// macOS (#759) completes synchronously and Android hands back an
	// AHardwareBuffer; ipc_handle_weave_get_fence has no non-Windows branch at
	// all, so gating on the fence there would mean NEVER latching.
	CHECK(oxr_weave_should_latch_export(false, /* tex */ true, /* fence */ false));
	CHECK(oxr_weave_should_latch_export(false, /* tex */ true, /* fence */ true));
}

TEST_CASE("weave export latch: Windows needs the fence too")
{
	// The caller cannot safely sample the woven texture without the fence, so a
	// texture-only export is a partial export: hand back what we have, but keep
	// asking so the fence still reaches the caller.
	CHECK_FALSE(oxr_weave_should_latch_export(true, /* tex */ true, /* fence */ false));
	CHECK(oxr_weave_should_latch_export(true, /* tex */ true, /* fence */ true));
}

TEST_CASE("weave export latch: the platform rule matches the build target")
{
#ifdef XRT_OS_WINDOWS
	CHECK(oxr_weave_platform_exports_fence());
#else
	CHECK_FALSE(oxr_weave_platform_exports_fence());
#endif
}
