// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two-mode contract applied to a head device (ADR-065): the trim,
 *         its WARN, and the OUTPUT_MODE index translation for a driver whose
 *         kept 3D mode is not its own index 1.
 * @ingroup aux_util
 */

#include "util/u_rendering_modes.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * OUTPUT_MODE translation for a trimmed driver.
 *
 * Needed only when the 3D mode the runtime kept was not at the driver's own
 * index 1 (e.g. a driver that started in its third mode). Callers hand
 * `XRT_DEVICE_PROPERTY_OUTPUT_MODE` the RUN-TIME index (array position); the
 * driver expects its own. The wrapper translates in both directions and
 * repoints the active index if the driver wrote its own number there.
 *
 * One head device per process in practice, a handful at most (an app that
 * creates and destroys instances): a small fixed table, keyed by device. An
 * entry is written before the wrapped function pointers are published on the
 * device and cleared by the wrapped destroy, after which nothing calls the
 * device; lookups of OTHER entries never race a write to this one.
 *
 */

#define U_RM_REMAP_MAX 4

struct u_rm_remap
{
	struct xrt_device *xdev;
	xrt_result_t (*set_property)(struct xrt_device *, enum xrt_device_property_id, int32_t);
	xrt_result_t (*get_property)(struct xrt_device *, enum xrt_device_property_id, int32_t *);
	void (*destroy)(struct xrt_device *);
	uint32_t to_driver[U_RENDERING_MODES_RUNTIME_COUNT];
	uint32_t from_driver[XRT_MAX_RENDERING_MODES];
	uint32_t driver_count;
};

static struct u_rm_remap g_remap[U_RM_REMAP_MAX];

static struct u_rm_remap *
remap_find(struct xrt_device *xdev)
{
	for (uint32_t i = 0; i < U_RM_REMAP_MAX; i++) {
		if (g_remap[i].xdev == xdev) {
			return &g_remap[i];
		}
	}
	return NULL;
}

static xrt_result_t
remap_set_property(struct xrt_device *xdev, enum xrt_device_property_id prop, int32_t value)
{
	struct u_rm_remap *r = remap_find(xdev);
	if (r == NULL || r->set_property == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	if (prop != XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
		return r->set_property(xdev, prop, value);
	}
	if (value < 0 || (uint32_t)value >= U_RENDERING_MODES_RUNTIME_COUNT) {
		return XRT_ERROR_NOT_IMPLEMENTED; // not a run-time mode
	}
	const uint32_t driver_idx = r->to_driver[value];
	xrt_result_t ret = r->set_property(xdev, prop, (int32_t)driver_idx);
	// A driver that tracks the active mode itself just wrote ITS index.
	if (xdev->hmd != NULL && driver_idx != (uint32_t)value &&
	    xdev->hmd->active_rendering_mode_index == driver_idx) {
		xdev->hmd->active_rendering_mode_index = (uint32_t)value;
	}
	return ret;
}

static xrt_result_t
remap_get_property(struct xrt_device *xdev, enum xrt_device_property_id prop, int32_t *out_value)
{
	struct u_rm_remap *r = remap_find(xdev);
	if (r == NULL || r->get_property == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	xrt_result_t ret = r->get_property(xdev, prop, out_value);
	if (ret == XRT_SUCCESS && prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE && out_value != NULL) {
		const int32_t d = *out_value;
		*out_value = (d >= 0 && (uint32_t)d < r->driver_count) ? (int32_t)r->from_driver[d]
		                                                       : (int32_t)U_RENDERING_MODE_2D;
	}
	return ret;
}

static void
remap_destroy(struct xrt_device *xdev)
{
	struct u_rm_remap *r = remap_find(xdev);
	void (*destroy)(struct xrt_device *) = (r != NULL) ? r->destroy : NULL;
	if (r != NULL) {
		memset(r, 0, sizeof(*r));
	}
	if (destroy != NULL) {
		destroy(xdev);
	}
}

static bool
remap_install(struct xrt_device *xdev, const struct u_rendering_modes_trim_info *info)
{
	struct u_rm_remap *r = remap_find(xdev);
	if (r != NULL) {
		// Already wrapped (a second enforce): refresh the maps only.
		memcpy(r->to_driver, info->to_driver, sizeof(r->to_driver));
		memcpy(r->from_driver, info->from_driver, sizeof(r->from_driver));
		r->driver_count = info->original_count;
		return true;
	}
	r = remap_find(NULL);
	if (r == NULL) {
		return false;
	}
	struct u_rm_remap e;
	memset(&e, 0, sizeof(e));
	e.set_property = xdev->set_property;
	e.get_property = xdev->get_property;
	e.destroy = xdev->destroy;
	memcpy(e.to_driver, info->to_driver, sizeof(e.to_driver));
	memcpy(e.from_driver, info->from_driver, sizeof(e.from_driver));
	e.driver_count = info->original_count;
	e.xdev = NULL;
	*r = e;
	r->xdev = xdev; // last: the entry is complete before it can be found

	xdev->set_property = remap_set_property;
	xdev->get_property = remap_get_property;
	xdev->destroy = remap_destroy;
	return true;
}


/*
 *
 * The apply.
 *
 */

enum u_rendering_modes_trim_result
u_device_enforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name)
{
	if (xdev == NULL || xdev->hmd == NULL) {
		return U_RENDERING_MODES_TRIM_UNCHANGED;
	}
	const char *name = (driver_name != NULL && driver_name[0] != '\0') ? driver_name : xdev->str;

	// Names of what will be dropped, captured before the table is compacted.
	char dropped[256];
	dropped[0] = '\0';
	struct xrt_rendering_mode before[XRT_MAX_RENDERING_MODES];
	const uint32_t before_count =
	    xdev->rendering_mode_count < XRT_MAX_RENDERING_MODES ? xdev->rendering_mode_count : XRT_MAX_RENDERING_MODES;
	memcpy(before, xdev->rendering_modes, sizeof(before));
	const uint32_t before_active = xdev->hmd->active_rendering_mode_index;

	struct u_rendering_modes_trim_info info;
	const enum u_rendering_modes_trim_result res = u_rendering_modes_trim(
	    xdev->rendering_modes, &xdev->rendering_mode_count, &xdev->hmd->active_rendering_mode_index, &info);

	if (res == U_RENDERING_MODES_TRIM_REFUSED) {
		U_LOG_W(
		    "Display driver '%s' declares %u rendering modes, but %s - the table is kept as it is and does NOT "
		    "meet the two-mode contract (ADR-065: [0] = 2D, [1] = 3D)",
		    name, before_count,
		    !u_rendering_mode_is_flat(&before[0]) ? "its mode 0 is not the 1-view 2D mode"
		                                          : "none of its modes is a 3D mode");
		return res;
	}

	if (res == U_RENDERING_MODES_TRIM_TRIMMED) {
		size_t used = 0;
		for (uint32_t i = 0; i < before_count; i++) {
			if ((info.dropped_mask & (1u << i)) == 0) {
				continue;
			}
			int w = snprintf(dropped + used, sizeof(dropped) - used, "%s%u '%s' (%u views)",
			                 used == 0 ? "" : ", ", i, before[i].mode_name, before[i].view_count);
			if (w < 0 || (size_t)w >= sizeof(dropped) - used) {
				break;
			}
			used += (size_t)w;
		}

		// The device's max view count follows the table it now serves: it is
		// what PRIMARY_MULTIVIEW_DXR reports and what the IPC proxy copies.
		const uint32_t max_views =
		    u_rendering_modes_max_view_count(xdev->rendering_modes, xdev->rendering_mode_count);
		if (max_views > 0) {
			xdev->hmd->view_count = max_views;
		}

		bool remapped = false;
		bool remap_failed = false;
		if (info.to_driver[1] != 1) {
			remapped = remap_install(xdev, &info);
			remap_failed = !remapped;
		}

		U_LOG_W(
		    "Display driver '%s' declares %u rendering modes; the runtime keeps two (ADR-065): 0 '%s' and 1 "
		    "'%s' "
		    "(the driver's mode %u, %u views). Dropped: %s. Active mode %u -> %u.%s",
		    name, before_count, xdev->rendering_modes[0].mode_name, xdev->rendering_modes[1].mode_name,
		    info.to_driver[1], xdev->rendering_modes[1].view_count, dropped[0] != '\0' ? dropped : "-",
		    before_active, xdev->hmd->active_rendering_mode_index,
		    remapped       ? " OUTPUT_MODE requests reach the driver in its own numbering."
		    : remap_failed ? " (no room to translate OUTPUT_MODE for this driver: it sees run-time indices.)"
		                   : "");
		return res;
	}

	if (info.renumbered) {
		U_LOG_W("Display driver '%s': rendering-mode mode_index renumbered to array positions (ADR-065)", name);
	}
	return res;
}
