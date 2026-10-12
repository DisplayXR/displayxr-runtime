// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Stub plug-in binaries for tests_conversion_loader (ADR-053).
 *
 * Built three times as test-only MODULE libraries:
 *  - STUB_KIND_CONVERSION, STUB_PLUGIN_API unset: a well-formed conversion
 *    provider at XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
 *  - STUB_KIND_CONVERSION, STUB_PLUGIN_API=<other major>: the same provider
 *    built against a different ABI major (the loader must reject it);
 *  - STUB_KIND_DISPLAY: a display-processor-shaped binary exporting only
 *    xrtPluginNegotiate (the conversion loader must not take it).
 * Never registered anywhere; the test hands the paths to the loader directly.
 */

#include "xrt/xrt_conversion_plugin.h"

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

// Shape only: the conversion loader must refuse this binary before calling it.
STUB_EXPORT int
xrtPluginNegotiate(void)
{
	return -1;
}

#else
#error "define STUB_KIND_CONVERSION or STUB_KIND_DISPLAY"
#endif
