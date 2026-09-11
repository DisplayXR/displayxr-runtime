// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The input-provider host iface's display-geometry cache (#1380).
 *
 * The host iface a provider is handed at `xrtInputPluginNegotiate` is
 * process-lifetime storage, and its `get_display_geometry` callback is valid
 * from that moment on — but the GEOMETRY behind it is only published once the
 * display plug-in has answered, which happens after negotiation and before any
 * provider's `create_devices`. Those two lifetimes are deliberately different,
 * and a provider is told them apart by a distinct result code rather than by a
 * zeroed struct.
 *
 * Driven through the very same function pointer a provider gets, so the test
 * cannot drift from what is actually handed out.
 */

#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_results.h"

#include "target_input_plugin_loader.h"

#include "catch_amalgamated.hpp"

#include <cstddef>
#include <cstring>


namespace {

//! A short struct_size: what a provider built against an older header sends.
constexpr uint32_t kPrefixSize =
    static_cast<uint32_t>(offsetof(struct xrt_input_host_display_geometry, nominal_viewer_x_m));

} // namespace


// ONE test case, run in order: the cache is process-global and set-once by
// contract, so "before" and "after" cannot be independent Catch2 sections.
TEST_CASE("input host display geometry")
{
	const struct xrt_input_plugin_host_iface *host = target_input_plugin_get_host_iface();
	REQUIRE(host != nullptr);

	// The persistent host storage is fully filled in from the first call,
	// which is what makes retaining the pointer legal (#1380).
	CHECK(host->struct_size == sizeof(struct xrt_input_plugin_host_iface));
	CHECK(host->host_api_version == XRT_INPUT_PLUGIN_API_VERSION_CURRENT);
	REQUIRE(host->get_display_geometry != nullptr);
	for (size_t i = 0; i < sizeof(host->reserved) / sizeof(host->reserved[0]); i++) {
		CHECK(host->reserved[i] == nullptr);
	}

	// The same pointer every time — providers keep it for the process life.
	CHECK(target_input_plugin_get_host_iface() == host);

	SECTION("the whole contract, in order")
	{
		/*
		 * 1. Before the builder publishes anything: a DISTINCT code,
		 *    and not one byte of the caller's struct is touched.
		 */
		struct xrt_input_host_display_geometry early = {};
		early.struct_size = (uint32_t)sizeof(early);
		early.display_width_m = -7.0f;
		early.display_height_m = -7.0f;
		early.nominal_viewer_x_m = -7.0f;
		early.nominal_viewer_y_m = -7.0f;
		early.nominal_viewer_z_m = -7.0f;

		CHECK(host->get_display_geometry(&early) == XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY);
		CHECK(early.struct_size == (uint32_t)sizeof(early));
		CHECK(early.display_width_m == -7.0f);
		CHECK(early.display_height_m == -7.0f);
		CHECK(early.nominal_viewer_x_m == -7.0f);
		CHECK(early.nominal_viewer_y_m == -7.0f);
		CHECK(early.nominal_viewer_z_m == -7.0f);

		// Garbage arguments are a different answer again, so "too
		// early" stays distinguishable from "you called it wrong".
		CHECK(host->get_display_geometry(nullptr) != XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY);
		CHECK(host->get_display_geometry(nullptr) != XRT_SUCCESS);

		struct xrt_input_host_display_geometry too_small = {};
		too_small.struct_size = 3; // Cannot even hold struct_size.
		CHECK(host->get_display_geometry(&too_small) != XRT_SUCCESS);
		CHECK(host->get_display_geometry(&too_small) != XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY);

		/*
		 * 2. The builder publishes the display plug-in's numbers.
		 */
		struct xrt_input_host_display_geometry published = {};
		published.struct_size = (uint32_t)sizeof(published);
		published.display_width_m = 0.6975f;
		published.display_height_m = 0.3924f;
		published.nominal_viewer_x_m = 0.0f;
		published.nominal_viewer_y_m = 0.05f;
		published.nominal_viewer_z_m = 0.65f;
		target_input_plugin_set_display_geometry(&published);

		/*
		 * 3. From here a provider gets the cached record.
		 */
		struct xrt_input_host_display_geometry got = {};
		got.struct_size = (uint32_t)sizeof(got);
		CHECK(host->get_display_geometry(&got) == XRT_SUCCESS);
		CHECK(got.struct_size == (uint32_t)sizeof(got));
		CHECK(got.display_width_m == Catch::Approx(0.6975f));
		CHECK(got.display_height_m == Catch::Approx(0.3924f));
		CHECK(got.nominal_viewer_x_m == Catch::Approx(0.0f));
		CHECK(got.nominal_viewer_y_m == Catch::Approx(0.05f));
		CHECK(got.nominal_viewer_z_m == Catch::Approx(0.65f));

		/*
		 * 4. Prefix semantics: an older provider's shorter struct gets
		 *    the fields it knows and nothing past its own size.
		 */
		struct xrt_input_host_display_geometry old_provider = {};
		old_provider.struct_size = kPrefixSize;
		old_provider.nominal_viewer_x_m = -7.0f;
		old_provider.nominal_viewer_y_m = -7.0f;
		old_provider.nominal_viewer_z_m = -7.0f;

		CHECK(host->get_display_geometry(&old_provider) == XRT_SUCCESS);
		CHECK(old_provider.struct_size == kPrefixSize); // Its own, kept.
		CHECK(old_provider.display_width_m == Catch::Approx(0.6975f));
		CHECK(old_provider.display_height_m == Catch::Approx(0.3924f));
		CHECK(old_provider.nominal_viewer_x_m == -7.0f); // Past its size.
		CHECK(old_provider.nominal_viewer_y_m == -7.0f);
		CHECK(old_provider.nominal_viewer_z_m == -7.0f);
	}
}
