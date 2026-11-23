// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two-mode contract applied to a head device (ADR-065): the trim,
 *         its WARN, re-enforcement, and the OUTPUT_MODE index translation for
 *         a driver whose kept 3D mode is not its own index 1.
 * @ingroup aux_util
 */

#include "util/u_rendering_modes.h"
#include "util/u_logging.h"

#include "os/os_threading.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Per-device record.
 *
 * Kept for every device the runtime TRIMMED (it declared more than two
 * modes): which driver index became run-time mode 1 (so a re-trim keeps the
 * same kind), the driver's own count, and - only when the kept index is not
 * the driver's 1 - the wrapped property / destroy functions.
 *
 * Why the wrapper shows the driver its own count. Callers hand
 * XRT_DEVICE_PROPERTY_OUTPUT_MODE the RUN-TIME index; the wrapper forwards
 * the driver's own index (e.g. 4). A pre-ADR-065 driver range-checks that
 * against xdev->rendering_mode_count and reads xdev->rendering_modes[value].
 * The trim leaves every entry >= 2 where the driver put it (only slot 1 is
 * overwritten, with a copy of the kept entry, and the forwarded index is never
 * 1 when the wrapper is installed), so the one thing the driver would see
 * wrong is the count. For the duration of the forwarded call - under the
 * record's lock - the count is the driver's own, and it is put back to 2
 * afterwards. Considered and rejected: a shadow xrt_device handed to the
 * driver. A driver casts the device to its own subclass and reads private
 * fields past the base struct, which a shadow copy of xrt_device cannot carry.
 *
 * Residual race, stated plainly: a reader on another thread that iterates the
 * table WITHOUT the lock during that call (the readers do not lock today) can
 * see the driver's count for its duration, i.e. two to N entries, all of them
 * the driver's real entries (slot 1 still holds the kept mode). Every reader
 * already bounds-checks the active index against the count, and the active
 * index is repointed BEFORE the count is restored. Only a driver whose kept
 * mode is not its index 1 is affected; a driver that declares two modes never
 * reaches this code.
 *
 * Threading assumption for enforce / re-enforce themselves: they run at
 * system creation (single-threaded) and from the display-info apply, which
 * its caller serialises (target_instance.c, g_display_info_mutex). The table
 * rewrite of a re-trim is not atomic against concurrent readers either - the
 * same is true of the plug-in's own rewrite that made a re-trim necessary.
 * Function pointers are written once per device, never re-written (a re-trim
 * only updates the maps, under the record's lock).
 *
 * One head device per process in practice, a handful at most (an app that
 * creates and destroys instances). A record whose wrappers are installed is
 * freed by the wrapped destroy; an unwrapped record cannot see its device die,
 * so the first enforce of a device drops any record left at the same address,
 * and unwrapped records are recycled when the table is full.
 *
 */

#define U_RM_RECORD_MAX 4

struct u_rm_record
{
	struct xrt_device *xdev;
	bool wrapped;
	bool lock_ready;
	struct os_mutex lock;

	//! The driver index that is run-time mode 1.
	uint32_t kept_driver_index;
	//! The driver's own count, shown to it during a forwarded call.
	uint32_t driver_count;
	uint32_t to_driver[U_RENDERING_MODES_RUNTIME_COUNT];
	uint32_t from_driver[XRT_MAX_RENDERING_MODES];

	xrt_result_t (*set_property)(struct xrt_device *, enum xrt_device_property_id, int32_t);
	xrt_result_t (*get_property)(struct xrt_device *, enum xrt_device_property_id, int32_t *);
	void (*destroy)(struct xrt_device *);
};

static struct u_rm_record g_records[U_RM_RECORD_MAX];
static uint32_t g_recycle_next = 0;

static struct u_rm_record *
record_find(struct xrt_device *xdev)
{
	for (uint32_t i = 0; i < U_RM_RECORD_MAX; i++) {
		if (g_records[i].xdev == xdev) {
			return &g_records[i];
		}
	}
	return NULL;
}

static void
record_clear(struct u_rm_record *r)
{
	if (r->lock_ready) {
		os_mutex_destroy(&r->lock);
	}
	memset(r, 0, sizeof(*r));
}

//! A free slot, or an unwrapped (recyclable) one; NULL when every slot wraps a live device.
static struct u_rm_record *
record_alloc(struct xrt_device *xdev)
{
	struct u_rm_record *r = record_find(NULL);
	if (r == NULL) {
		for (uint32_t n = 0; n < U_RM_RECORD_MAX && r == NULL; n++) {
			struct u_rm_record *c = &g_records[(g_recycle_next + n) % U_RM_RECORD_MAX];
			if (!c->wrapped) {
				r = c;
				g_recycle_next = (g_recycle_next + n + 1) % U_RM_RECORD_MAX;
			}
		}
		if (r == NULL) {
			return NULL;
		}
		record_clear(r);
	}
	memset(r, 0, sizeof(*r));
	r->lock_ready = os_mutex_init(&r->lock) == 0;
	r->xdev = xdev;
	return r;
}

static void
record_set_maps(struct u_rm_record *r, const struct u_rendering_modes_trim_info *info)
{
	r->kept_driver_index = info->to_driver[1];
	r->driver_count = info->original_count;
	memcpy(r->to_driver, info->to_driver, sizeof(r->to_driver));
	memcpy(r->from_driver, info->from_driver, sizeof(r->from_driver));
}

static void
record_lock(struct u_rm_record *r)
{
	if (r->lock_ready) {
		os_mutex_lock(&r->lock);
	}
}

static void
record_unlock(struct u_rm_record *r)
{
	if (r->lock_ready) {
		os_mutex_unlock(&r->lock);
	}
}

//! Show the driver its own count for one forwarded call (record lock held).
static uint32_t
driver_view_begin(struct u_rm_record *r, struct xrt_device *xdev)
{
	const uint32_t served = xdev->rendering_mode_count;
	if (r->driver_count > served && r->driver_count <= XRT_MAX_RENDERING_MODES) {
		xdev->rendering_mode_count = r->driver_count;
	}
	return served;
}

static void
driver_view_end(struct xrt_device *xdev, uint32_t served)
{
	xdev->rendering_mode_count = served;
}

static xrt_result_t
remap_set_property(struct xrt_device *xdev, enum xrt_device_property_id prop, int32_t value)
{
	struct u_rm_record *r = record_find(xdev);
	if (r == NULL || r->set_property == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	if (prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE &&
	    (value < 0 || (uint32_t)value >= U_RENDERING_MODES_RUNTIME_COUNT)) {
		return XRT_ERROR_NOT_IMPLEMENTED; // not a run-time mode
	}

	record_lock(r);
	int32_t forwarded = value;
	if (prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE) {
		forwarded = (int32_t)r->to_driver[value];
	}
	const uint32_t served = driver_view_begin(r, xdev);
	xrt_result_t ret = r->set_property(xdev, prop, forwarded);
	// A driver that tracks the active mode itself just wrote ITS index:
	// repoint it BEFORE the served count comes back, so a reader never sees an
	// active index past the count.
	if (prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE && xdev->hmd != NULL) {
		const uint32_t a = xdev->hmd->active_rendering_mode_index;
		if (a >= U_RENDERING_MODES_RUNTIME_COUNT && a < XRT_MAX_RENDERING_MODES && a < r->driver_count) {
			xdev->hmd->active_rendering_mode_index = r->from_driver[a];
		}
	}
	driver_view_end(xdev, served);
	record_unlock(r);
	return ret;
}

static xrt_result_t
remap_get_property(struct xrt_device *xdev, enum xrt_device_property_id prop, int32_t *out_value)
{
	struct u_rm_record *r = record_find(xdev);
	if (r == NULL || r->get_property == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	record_lock(r);
	const uint32_t served = driver_view_begin(r, xdev);
	xrt_result_t ret = r->get_property(xdev, prop, out_value);
	driver_view_end(xdev, served);
	if (ret == XRT_SUCCESS && prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE && out_value != NULL) {
		const int32_t d = *out_value;
		*out_value = (d >= 0 && (uint32_t)d < r->driver_count && d < XRT_MAX_RENDERING_MODES)
		                 ? (int32_t)r->from_driver[d]
		                 : (int32_t)U_RENDERING_MODE_2D;
	}
	record_unlock(r);
	return ret;
}

static void
remap_destroy(struct xrt_device *xdev)
{
	struct u_rm_record *r = record_find(xdev);
	void (*destroy)(struct xrt_device *) = (r != NULL) ? r->destroy : NULL;
	if (r != NULL) {
		record_clear(r);
	}
	if (destroy != NULL) {
		destroy(xdev);
	}
}

//! Install the wrappers, once per device. False when already installed.
static bool
record_wrap(struct u_rm_record *r, struct xrt_device *xdev)
{
	if (r->wrapped) {
		return false;
	}
	r->set_property = xdev->set_property;
	r->get_property = xdev->get_property;
	r->destroy = xdev->destroy;
	r->wrapped = true;
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

static void
log_trim(const char *name,
         const struct xrt_rendering_mode *before,
         uint32_t before_count,
         uint32_t before_active,
         const struct xrt_device *xdev,
         const struct u_rendering_modes_trim_info *info,
         bool wrapped,
         bool no_room,
         bool again)
{
	char dropped[256];
	dropped[0] = '\0';
	size_t used = 0;
	for (uint32_t i = 0; i < before_count; i++) {
		if ((info->dropped_mask & (1u << i)) == 0) {
			continue;
		}
		int w = snprintf(dropped + used, sizeof(dropped) - used, "%s%u '%s' (%u views)", used == 0 ? "" : ", ",
		                 i, before[i].mode_name, before[i].view_count);
		if (w < 0 || (size_t)w >= sizeof(dropped) - used) {
			break;
		}
		used += (size_t)w;
	}
	U_LOG_W(
	    "Display driver '%s' declares %u rendering modes; the runtime keeps two (ADR-065)%s: 0 '%s' and 1 '%s' "
	    "(the driver's mode %u, %u views). Dropped: %s. Active mode %u -> %u.%s",
	    name, before_count, again ? " again (the driver rewrote its table)" : "",
	    xdev->rendering_modes[0].mode_name, xdev->rendering_modes[1].mode_name, info->to_driver[1],
	    xdev->rendering_modes[1].view_count, dropped[0] != '\0' ? dropped : "-", before_active,
	    xdev->hmd->active_rendering_mode_index,
	    wrapped   ? " OUTPUT_MODE requests reach the driver in its own numbering."
	    : no_room ? " (no room to translate OUTPUT_MODE for this driver: it sees run-time indices.)"
	              : "");
}

static enum u_rendering_modes_trim_result
enforce(struct xrt_device *xdev, const char *driver_name, bool refresh)
{
	if (xdev == NULL || xdev->hmd == NULL) {
		return U_RENDERING_MODES_TRIM_UNCHANGED;
	}
	const char *name = (driver_name != NULL && driver_name[0] != '\0') ? driver_name : xdev->str;

	struct u_rm_record *rec = record_find(xdev);
	// A record at this address that does not wrap THIS device's functions is
	// left over from a destroyed device: a first enforce starts afresh.
	const bool rec_live = rec != NULL && (refresh || (rec->wrapped && xdev->set_property == remap_set_property));
	if (rec != NULL && !rec_live) {
		record_clear(rec);
		rec = NULL;
	}

	const uint32_t before_count =
	    xdev->rendering_mode_count < XRT_MAX_RENDERING_MODES ? xdev->rendering_mode_count : XRT_MAX_RENDERING_MODES;
	if (before_count <= U_RENDERING_MODES_RUNTIME_COUNT) {
		// Nothing to trim (the common case, and every refresh after a trim).
		struct u_rendering_modes_trim_info info;
		uint32_t count = xdev->rendering_mode_count;
		(void)u_rendering_modes_trim(xdev->rendering_modes, &count, NULL, &info);
		if (info.renumbered) {
			U_LOG_W(
			    "Display driver '%s': rendering-mode mode_index renumbered to array positions (ADR-065)",
			    name);
		}
		return U_RENDERING_MODES_TRIM_UNCHANGED;
	}

	struct xrt_rendering_mode before[XRT_MAX_RENDERING_MODES];
	memcpy(before, xdev->rendering_modes, sizeof(before));
	const uint32_t before_active = xdev->hmd->active_rendering_mode_index;

	// Which driver index the pick should prefer. A re-trim (the driver
	// rewrote its full table after the first one) keeps the SAME kind when it
	// is still a 3D mode: the active index is in RUN-TIME numbering by now and
	// means nothing in the driver's. A first trim uses the active index, which
	// is still the driver's.
	uint32_t steer = before_active;
	bool again = false;
	if (rec != NULL && rec->kept_driver_index >= 1 && rec->kept_driver_index < before_count) {
		again = true;
		steer = u_rendering_mode_is_3d(&before[rec->kept_driver_index]) ? rec->kept_driver_index : 0;
	}

	uint32_t steer_inout = steer;
	struct u_rendering_modes_trim_info info;
	const enum u_rendering_modes_trim_result res =
	    u_rendering_modes_trim(xdev->rendering_modes, &xdev->rendering_mode_count, &steer_inout, &info);

	if (res == U_RENDERING_MODES_TRIM_REFUSED) {
		U_LOG_W(
		    "Display driver '%s' declares %u rendering modes, but %s - the table is kept as it is and does NOT "
		    "meet the two-mode contract (ADR-065: [0] = 2D, [1] = 3D)",
		    name, before_count,
		    !u_rendering_mode_is_flat(&before[0]) ? "its mode 0 is not the 1-view 2D mode"
		                                          : "none of its modes is a 3D mode");
		return res;
	}

	// The active index the runtime serves.
	if (!again) {
		xdev->hmd->active_rendering_mode_index = steer_inout; // translated by the trim
	} else if (before_active >= U_RENDERING_MODES_RUNTIME_COUNT) {
		// The driver's own rewrite also wrote its own active index.
		xdev->hmd->active_rendering_mode_index =
		    before_active < XRT_MAX_RENDERING_MODES ? info.from_driver[before_active] : U_RENDERING_MODE_2D;
	} // else: already a run-time index (0 or 1): it keeps its meaning.

	// The device's max view count follows the table it now serves: it is what
	// PRIMARY_MULTIVIEW_DXR reports and what the IPC proxy copies.
	const uint32_t max_views = u_rendering_modes_max_view_count(xdev->rendering_modes, xdev->rendering_mode_count);
	if (max_views > 0) {
		xdev->hmd->view_count = max_views;
	}

	bool wrapped = false;
	bool no_room = false;
	if (rec == NULL) {
		rec = record_alloc(xdev);
	}
	if (rec != NULL) {
		record_lock(rec);
		record_set_maps(rec, &info);
		record_unlock(rec);
		if (info.to_driver[1] != 1) {
			// Installed at most once per device; a re-trim only updated the
			// maps above.
			(void)record_wrap(rec, xdev);
		}
		wrapped = rec->wrapped;
	} else {
		no_room = info.to_driver[1] != 1;
	}

	log_trim(name, before, before_count, before_active, xdev, &info, wrapped, no_room, again);
	return res;
}

enum u_rendering_modes_trim_result
u_device_enforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name)
{
	return enforce(xdev, driver_name, false);
}

enum u_rendering_modes_trim_result
u_device_reenforce_two_rendering_modes(struct xrt_device *xdev, const char *driver_name)
{
	return enforce(xdev, driver_name, true);
}

uint32_t
u_device_rendering_modes_kept_driver_index(struct xrt_device *xdev)
{
	struct u_rm_record *r = record_find(xdev);
	return (r != NULL && xdev != NULL) ? r->kept_driver_index : 0;
}
