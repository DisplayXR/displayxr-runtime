// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Stub conversion providers for tests_conv_snapshot (ADR-053, #2006).
 *
 * Built as test-only MODULE libraries, one per variant:
 *  - STUB_VARIANT_GOOD:      two well-formed capabilities (DEPTH_VIDEO stream,
 *                            VIEWS_MONO stream with EXPLICIT_VIEWPOINTS);
 *  - STUB_VARIANT_MALFORMED: a VIEWS_MONO capability WITHOUT
 *                            EXPLICIT_VIEWPOINTS (selftest exit 13);
 *  - STUB_VARIANT_BAD_STATE: a well-formed capability whose get_state answers
 *                            an out-of-range state (selftest exit 13).
 * Never registered anywhere; the test hands the paths to the headless pass.
 */

#include "xrt/xrt_conversion_plugin.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define STUB_EXPORT __declspec(dllexport)
#else
#define STUB_EXPORT __attribute__((visibility("default")))
#endif

#if defined(STUB_VARIANT_GOOD)
#define STUB_ID "stub-good"
#define STUB_CAP_COUNT 2u
#elif defined(STUB_VARIANT_MALFORMED)
#define STUB_ID "stub-malformed"
#define STUB_CAP_COUNT 1u
#elif defined(STUB_VARIANT_BAD_STATE)
#define STUB_ID "stub-bad-state"
#define STUB_CAP_COUNT 1u
#else
#error "define STUB_VARIANT_GOOD, STUB_VARIANT_MALFORMED or STUB_VARIANT_BAD_STATE"
#endif

static bool
stub_status(struct xrt_conv_provider_status *out)
{
	out->state = 1; // READY
	return true;
}

static xrt_result_t
stub_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	*out_inst = NULL;
	return XRT_SUCCESS;
}

static void
fill(uint32_t index, struct xrt_conv_capability *c)
{
	c->cap_id = index + 1;
	c->lanes = XRT_CONV_LANE_STREAM;
	c->input_kinds = XRT_CONV_INPUT_MONO;
	c->licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	c->commercial_ok = 1;
	strcpy(c->licence_spdx, "MIT");
	strcpy(c->backend, "stub-cpu");
	strcpy(c->model_id, "stub-model");
	c->est_latency_ms = 7;
#if defined(STUB_VARIANT_GOOD)
	if (index == 0) {
		c->task = XRT_CONV_TASK_DEPTH_VIDEO;
		c->outputs = XRT_CONV_OUT_DEPTH;
		c->flags = XRT_CONV_CAP_TEMPORAL;
	} else {
		c->task = XRT_CONV_TASK_VIEWS_MONO;
		c->outputs = XRT_CONV_OUT_VIEWS;
		c->flags = XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		c->min_views = 2;
		c->max_views = 8;
	}
#elif defined(STUB_VARIANT_MALFORMED)
	c->task = XRT_CONV_TASK_VIEWS_MONO;
	c->outputs = XRT_CONV_OUT_VIEWS;
	c->flags = 0; // MALFORMED: VIEWS_* must honour explicit viewpoints
	c->min_views = 2;
	c->max_views = 8;
#else
	c->task = XRT_CONV_TASK_DEPTH_MONO;
	c->outputs = XRT_CONV_OUT_DEPTH;
#endif
}

static xrt_result_t
stub_describe(struct xrt_conversion_plugin_instance *inst,
              uint32_t index,
              struct xrt_conv_capability *out,
              uint32_t *out_count)
{
	(void)inst;
	if (out_count != NULL) {
		*out_count = STUB_CAP_COUNT;
	}
	if (out == NULL) {
		return XRT_SUCCESS;
	}
	if (index >= STUB_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill(index, out);
	return XRT_SUCCESS;
}

static uint64_t
stub_generation(struct xrt_conversion_plugin_instance *inst)
{
	(void)inst;
	return 1;
}

static xrt_result_t
stub_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	(void)inst;
	(void)cap_id;
#if defined(STUB_VARIANT_BAD_STATE)
	out->state = 42; // out of range
#else
	out->state = XRT_CONV_STATE_INSTALLED;
#endif
	return XRT_SUCCESS;
}

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
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	memset(&g_iface, 0, sizeof(g_iface));
	g_iface.struct_size = (uint32_t)sizeof(g_iface);
	g_iface.id = STUB_ID;
	g_iface.display_name = "Stub (tests_conv_snapshot)";
	g_iface.get_provider_status = stub_status;
	g_iface.probe = stub_probe;
	g_iface.describe_cap = stub_describe;
	g_iface.get_generation = stub_generation;
	g_iface.get_state = stub_get_state;
	g_iface.destroy = stub_destroy;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
