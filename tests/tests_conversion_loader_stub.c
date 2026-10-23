// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Stub plug-in binaries for the loader tests and the per-class
 *        conformance gate (ADR-053, ADR-054 N4).
 *
 * Built several times as test-only MODULE libraries, one class per kind:
 *  - STUB_KIND_CONVERSION, STUB_PLUGIN_API unset: a well-formed conversion
 *    provider at XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
 *  - STUB_KIND_CONVERSION, STUB_PLUGIN_API=<other major>: the same provider
 *    built against a different ABI major (the loader must reject it);
 *  - STUB_KIND_DISPLAY: a display-processor-shaped binary exporting only
 *    xrtPluginNegotiate (the conversion loader must not take it; the DP
 *    loader sees a clean decline);
 *  - STUB_KIND_DISPLAY, STUB_PLUGIN_API=<other major>: the ABI-mismatch
 *    fixture for the DP class — negotiate reports the foreign major;
 *  - STUB_KIND_TRACKING + STUB_PLUGIN_API: the viewer-tracking class's
 *    ABI-mismatch twin (ADR-054 N6 Part 2), for the same N4 cell.
 *  - STUB_KIND_INPUT [+ STUB_PLUGIN_API]: the same two shapes for the
 *    input-provider class.
 *
 * Never registered by anything that ships. The unit tests hand the paths to
 * the loaders directly; CI's conformance step registers the ABI fixtures
 * under HKLM for one self-test run and removes them again, to prove that an
 * ABI-major mismatch is rejected and fallen through — never dispatched —
 * for every class (ADR-020 rule 3, ADR-054 §2.1).
 */

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_tracking_plugin.h"

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define STUB_EXPORT __declspec(dllexport)
#else
#define STUB_EXPORT __attribute__((visibility("default")))
#endif

#if defined(STUB_KIND_CONVERSION)

#ifndef STUB_PLUGIN_API
#define STUB_PLUGIN_API XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT
#endif

static void
stub_destroy(struct xrt_conversion_plugin_instance *inst)
{
	(void)inst;
}

static struct xrt_conversion_plugin_iface g_iface;

STUB_EXPORT xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	(void)host;
	*out_plugin_api_version = STUB_PLUGIN_API;
	if (runtime_api_version != STUB_PLUGIN_API) {
		*out_iface = NULL;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_iface.struct_size = (uint32_t)sizeof(g_iface);
	g_iface.id = "stub-conversion";
	g_iface.display_name = "Stub Conversion (test)";
	g_iface.destroy = stub_destroy;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}

#elif defined(STUB_KIND_DISPLAY)

#ifndef STUB_PLUGIN_API
#define STUB_PLUGIN_API XRT_PLUGIN_API_VERSION_CURRENT
#endif

/*
 * Real signature. At the runtime's own major it DECLINES (the loader logs and
 * moves on — a display processor for hardware that is not here). At a foreign
 * major it does what a plug-in built against other headers does: hands back
 * a vtable and reports its own major — and the loader must reject it on the
 * version alone, before reading one byte of that vtable (ADR-020 rule 3).
 * The vtable is an empty struct whose size is the only thing set, so a loader
 * that DID dispatch would fault on a NULL slot rather than run anything.
 */
static struct xrt_plugin_iface g_dp_foreign_iface;

STUB_EXPORT xrt_result_t
xrtPluginNegotiate(uint32_t runtime_api_version,
                   const struct xrt_plugin_host_iface *host,
                   struct xrt_plugin_iface **out_iface,
                   uint32_t *out_plugin_api_version)
{
	(void)host;
	*out_iface = NULL;
	*out_plugin_api_version = STUB_PLUGIN_API;
	if (runtime_api_version != STUB_PLUGIN_API) {
		g_dp_foreign_iface.struct_size = (uint32_t)sizeof(g_dp_foreign_iface);
		*out_iface = &g_dp_foreign_iface;
		return XRT_SUCCESS;
	}
	return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

#elif defined(STUB_KIND_INPUT)

#ifndef STUB_PLUGIN_API
#define STUB_PLUGIN_API XRT_INPUT_PLUGIN_API_VERSION_CURRENT
#endif

// Same two shapes as the display stub, for the input-provider class.
static struct xrt_input_plugin_iface g_input_foreign_iface;

STUB_EXPORT xrt_result_t
xrtInputPluginNegotiate(uint32_t runtime_api_version,
                        const struct xrt_input_plugin_host_iface *host,
                        struct xrt_input_plugin_iface **out_iface,
                        uint32_t *out_plugin_api_version)
{
	(void)host;
	*out_iface = NULL;
	*out_plugin_api_version = STUB_PLUGIN_API;
	if (runtime_api_version != STUB_PLUGIN_API) {
		g_input_foreign_iface.struct_size = (uint32_t)sizeof(g_input_foreign_iface);
		*out_iface = &g_input_foreign_iface;
		return XRT_SUCCESS;
	}
	return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

#elif defined(STUB_KIND_TRACKING)

#ifndef STUB_PLUGIN_API
#define STUB_PLUGIN_API XRT_TRACKING_PLUGIN_API_VERSION_CURRENT
#endif

// ADR-054 N6 Part 2: the same two shapes for the viewer-tracking class.
static struct xrt_tracking_plugin_iface g_tracking_foreign_iface;

STUB_EXPORT xrt_result_t
xrtTrackingPluginNegotiate(uint32_t runtime_api_version,
                           const struct xrt_tracking_plugin_host_iface *host,
                           struct xrt_tracking_plugin_iface **out_iface,
                           uint32_t *out_plugin_api_version)
{
	(void)host;
	*out_iface = NULL;
	*out_plugin_api_version = STUB_PLUGIN_API;
	if (runtime_api_version != STUB_PLUGIN_API) {
		g_tracking_foreign_iface.struct_size = (uint32_t)sizeof(g_tracking_foreign_iface);
		*out_iface = &g_tracking_foreign_iface;
		return XRT_SUCCESS;
	}
	return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

#else
#error "define STUB_KIND_CONVERSION, STUB_KIND_DISPLAY, STUB_KIND_INPUT or STUB_KIND_TRACKING"
#endif
