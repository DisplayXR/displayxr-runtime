// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two-slot contract applied to a head device (ADR-065, ADR-066
 *         C1): the selection, its WARN, re-enforcement, live config remaps,
 *         and the OUTPUT_MODE index translation for a driver that declares
 *         more than two modes.
 * @ingroup aux_util
 */

#include "util/u_rendering_modes.h"
#include "util/u_logging.h"
#include "util/u_setting.h"

#include "xrt/xrt_display_status.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include <stdio.h>
#include <string.h>


/*
 *
 * Per-device record.
 *
 * Kept for every device whose slots the runtime SELECTED (it declared more
 * than two modes): which driver index slot 1 maps to, the driver's own count
 * (the envelope), the slot-table generation and the wrapped property /
 * destroy functions. An IPC client's head proxy has an UNWRAPPED record
 * instead (u_device_rendering_modes_publish): its envelope count, its
 * generation and its sync hook.
 *
 * The array layout (u_rendering_modes.h): `[0, envelope)` is the declared list
 * permuted by the transposition (1, kept). The wrapper shows the driver its
 * own table for each forwarded call - its count, and the transposition
 * undone - under the record's lock, and puts the presented view back before
 * the call returns. Considered and rejected: a shadow xrt_device handed to the
 * driver. A driver casts the device to its own subclass and reads private
 * fields past the base struct, which a shadow copy of xrt_device cannot carry.
 *
 * Residual race, stated plainly: a reader on another thread that iterates the
 * table WITHOUT the lock during a forwarded call (the readers do not lock) can
 * see the driver's count and order for its duration - all of them the
 * driver's real entries. Every reader already bounds-checks the active index
 * against the count, and the active index is repointed BEFORE the presented
 * view comes back. A live remap swaps two entries in place with the same
 * exposure. Only a driver that declares more than two modes is affected; a
 * driver that declares two never reaches this code.
 *
 * Threading for enforce / re-enforce: they run at system creation
 * (single-threaded) and from the display-info apply, which its caller
 * serialises (target_instance.c, g_display_info_mutex). A config refresh runs
 * on one thread per process (the state tracker's poll, or the service's
 * component-watch thread) and takes the record's lock. Function pointers are
 * written once per device, never re-written.
 *
 * One head device per process in practice, a handful at most (an app that
 * creates and destroys instances). A record whose wrappers are installed is
 * freed by the wrapped destroy; an IPC proxy's record by
 * u_device_rendering_modes_forget; an unwrapped record that nobody forgot is
 * dropped by the first enforce of a device at the same address, and unwrapped
 * records are recycled when the table is full.
 *
 */

#define U_RM_RECORD_MAX 4

//! How often an in-process device's configuration is re-read (ADR-066 OQ 8).
#define U_RM_CONFIG_POLL_NS (1000ull * 1000ull * 1000ull)

struct u_rm_record
{
	struct xrt_device *xdev;
	bool wrapped;
	bool lock_ready;
	struct os_mutex lock;

	//! The driver index that is run-time mode 1.
	uint32_t kept_driver_index;
	//! The driver's own count (every entry it listed), shown to it during a forwarded call.
	uint32_t driver_count;
	//! The sizing envelope: driver_count without trailing start-up-only listings.
	uint32_t envelope_count;
	uint32_t to_driver[U_RENDERING_MODES_RUNTIME_COUNT];
	uint32_t from_driver[XRT_MAX_RENDERING_MODES];

	//! Bumped on every remap (ADR-066 C1.5).
	volatile uint32_t generation;
	//! Last config poll (u_device_rendering_modes_sync), monotonic ns; 0 = never.
	uint64_t last_poll_ns;
	//! The per-user store stamp at the last config re-read (u_setting_user_stamp).
	uint64_t store_stamp;
	//! The last refused CONFIGURED_3D_MODE answer, so it is WARNed once.
	int32_t last_refused;
	//! The last per-screen choice that named no declared 3D entry (WARNed once).
	char last_refused_choice[32];
	char name[64];

	//! What chose slot 1 (`enum xrt_status_mode_3d_source`) and the stored choice it honoured.
	uint32_t config_source;
	char setting[32];

	//! IPC proxies: make the table current, return the generation.
	u_rendering_modes_sync_fn sync_fn;

	xrt_result_t (*set_property)(struct xrt_device *, enum xrt_device_property_id, int32_t);
	xrt_result_t (*get_property)(struct xrt_device *, enum xrt_device_property_id, int32_t *);
	void (*destroy)(struct xrt_device *);
};

static struct u_rm_record g_records[U_RM_RECORD_MAX];
static uint32_t g_recycle_next = 0;
static u_rendering_modes_choice_fn g_choice_resolver = NULL;

//! Where driver entry @p j sits in a device array permuted by (1, @p kept).
static uint32_t
pos_of(uint32_t j, uint32_t kept)
{
	if (kept <= 1) {
		return j;
	}
	return j == 1 ? kept : (j == kept ? 1u : j);
}

//! The driver index of the 3D entry @p choice names in a permuted array; 0 = none.
static uint32_t
find_choice(const struct xrt_rendering_mode *modes, uint32_t n, uint32_t kept, const char *choice)
{
	for (uint32_t j = 1; j < n && j < XRT_MAX_RENDERING_MODES; j++) {
		const struct xrt_rendering_mode *m = &modes[pos_of(j, kept)];
		if (u_rendering_mode_is_selectable(m) && u_rendering_mode_matches_choice(m, choice)) {
			return j;
		}
	}
	return 0;
}

/*!
 * The remap's maps over EVERY listed entry (not just the envelope) for a
 * permuted array whose slot 1 is now driver entry @p next (array positions
 * already swapped for @p next).
 */
static void
u_rendering_modes_fill_maps_listed(const struct xrt_rendering_mode *modes,
                                   uint32_t listed,
                                   uint32_t prev_kept,
                                   uint32_t next,
                                   struct u_rendering_modes_trim_info *info)
{
	(void)prev_kept;
	info->to_driver[0] = 0;
	info->to_driver[1] = next;
	for (uint32_t i = 0; i < XRT_MAX_RENDERING_MODES; i++) {
		if (i == 0) {
			info->from_driver[i] = U_RENDERING_MODE_2D;
		} else if (i == next) {
			info->from_driver[i] = U_RENDERING_MODE_3D;
		} else if (i < listed) {
			info->from_driver[i] =
			    u_rendering_mode_is_3d(&modes[pos_of(i, next)]) ? U_RENDERING_MODE_3D : U_RENDERING_MODE_2D;
		} else {
			info->from_driver[i] = U_RENDERING_MODE_2D;
		}
	}
}

//! Ask the process's resolver for @p xdev's per-screen choice.
static bool
resolve_choice(struct xrt_device *xdev, char *name, size_t cap, uint32_t *src)
{
	name[0] = '\0';
	*src = XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	u_rendering_modes_choice_fn fn = g_choice_resolver;
	return fn != NULL && fn(xdev, name, cap, src) && name[0] != '\0';
}

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
	r->last_refused = -1;
	return r;
}

static void
record_set_maps(struct u_rm_record *r, const struct u_rendering_modes_trim_info *info)
{
	r->kept_driver_index = info->to_driver[1];
	r->driver_count = info->original_count;
	r->envelope_count = info->envelope_count;
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

//! Show the driver its own table for one forwarded call (record lock held).
static uint32_t
driver_view_begin(struct u_rm_record *r, struct xrt_device *xdev)
{
	const uint32_t served = xdev->rendering_mode_count;
	if (r->driver_count > served && r->driver_count <= XRT_MAX_RENDERING_MODES) {
		xdev->rendering_mode_count = r->driver_count;
		if (r->kept_driver_index > 1 && r->kept_driver_index < r->driver_count) {
			u_rendering_modes_swap(xdev->rendering_modes, 1, r->kept_driver_index);
		}
	}
	return served;
}

static void
driver_view_end(struct u_rm_record *r, struct xrt_device *xdev, uint32_t served)
{
	if (xdev->rendering_mode_count != served) {
		if (r->kept_driver_index > 1 && r->kept_driver_index < r->driver_count) {
			u_rendering_modes_swap(xdev->rendering_modes, 1, r->kept_driver_index);
		}
		xdev->rendering_mode_count = served;
	}
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
	// repoint it BEFORE the presented view comes back, so a reader never sees
	// an active index past the count.
	if (prop == XRT_DEVICE_PROPERTY_OUTPUT_MODE && xdev->hmd != NULL) {
		const uint32_t a = xdev->hmd->active_rendering_mode_index;
		if (a >= 1 && a < XRT_MAX_RENDERING_MODES && a < r->driver_count) {
			xdev->hmd->active_rendering_mode_index = r->from_driver[a];
		}
	}
	driver_view_end(r, xdev, served);
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
	driver_view_end(r, xdev, served);
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

//! The driver's configured slot-1 entry, asked directly (before / without the wrapper).
static uint32_t
query_configured(xrt_result_t (*get_property)(struct xrt_device *, enum xrt_device_property_id, int32_t *),
                 struct xrt_device *xdev)
{
	if (get_property == NULL) {
		return U_RENDERING_MODES_NO_PREFERENCE;
	}
	int32_t v = -1;
	if (get_property(xdev, XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE, &v) != XRT_SUCCESS || v < 1) {
		return U_RENDERING_MODES_NO_PREFERENCE;
	}
	return (uint32_t)v;
}


/*
 *
 * The apply.
 *
 */

static void
log_trim(const char *name,
         uint32_t before_count,
         uint32_t before_active,
         const struct xrt_device *xdev,
         const struct u_rendering_modes_trim_info *info,
         bool wrapped,
         bool no_room,
         bool again)
{
	char kept[256];
	kept[0] = '\0';
	size_t used = 0;
	for (uint32_t i = U_RENDERING_MODES_RUNTIME_COUNT; i < info->envelope_count; i++) {
		int w = snprintf(kept + used, sizeof(kept) - used, "%s'%s' (%u views)", used == 0 ? "" : ", ",
		                 xdev->rendering_modes[i].mode_name, xdev->rendering_modes[i].view_count);
		if (w < 0 || (size_t)w >= sizeof(kept) - used) {
			break;
		}
		used += (size_t)w;
	}
	U_LOG_W(
	    "Display driver '%s' declares %u rendering modes%s; apps see two slots (ADR-065): 0 '%s' and 1 '%s' "
	    "(the driver's mode %u, %u views). The full list is kept as the sizing envelope (ADR-066 C1), "
	    "other entries: %s. Active mode %u -> %u.%s",
	    name, before_count, again ? " again (the driver rewrote its table)" : "",
	    xdev->rendering_modes[0].mode_name, xdev->rendering_modes[1].mode_name, info->to_driver[1],
	    xdev->rendering_modes[1].view_count, kept[0] != '\0' ? kept : "-", before_active,
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
		// Nothing to select (the common case, and every refresh after a
		// selection: the presented count is 2).
		struct u_rendering_modes_trim_info info;
		uint32_t count = xdev->rendering_mode_count;
		(void)u_rendering_modes_trim(xdev->rendering_modes, &count, NULL, U_RENDERING_MODES_NO_PREFERENCE,
		                             &info);
		if (info.renumbered) {
			U_LOG_W(
			    "Display driver '%s': rendering-mode mode_index renumbered to array positions (ADR-065)",
			    name);
		}
		return U_RENDERING_MODES_TRIM_UNCHANGED;
	}

	const uint32_t before_active = xdev->hmd->active_rendering_mode_index;

	// ADR-066 C1.1: an entry wider than XRT_MAX_VIEWS cannot be presented: it
	// is never selected and never sizes the view count (one WARN per entry).
	for (uint32_t i = 0; i < before_count; i++) {
		if (xdev->rendering_modes[i].view_count > XRT_MAX_VIEWS) {
			U_LOG_W(
			    "Display driver '%s': rendering mode %u '%s' has %u views, more than the %u that can be "
			    "presented - it is never selected (ADR-066 C1)",
			    name, i, xdev->rendering_modes[i].mode_name, xdev->rendering_modes[i].view_count,
			    (unsigned)XRT_MAX_VIEWS);
		}
	}

	// Which driver index slot 1 should map to. A re-selection (the driver
	// rewrote its full table after the first one) keeps the SAME entry when
	// it is still a 3D entry: the active index is in RUN-TIME numbering by
	// now and means nothing in the driver's. A first selection asks the
	// driver's configuration, then falls back to the active index, which is
	// still the driver's.
	uint32_t steer = before_active;
	uint32_t preferred = U_RENDERING_MODES_NO_PREFERENCE;
	bool again = false;
	if (rec != NULL && rec->kept_driver_index >= 1 && rec->kept_driver_index < before_count) {
		again = true;
		preferred = rec->kept_driver_index;
		steer = 0;
	}
	uint32_t first_source = XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	char first_setting[32] = {0};
	if (!again) {
		uint32_t csrc = 0;
		char choice[32];
		const uint32_t by_choice = resolve_choice(xdev, choice, sizeof(choice), &csrc)
		                               ? find_choice(xdev->rendering_modes, before_count, 1, choice)
		                               : 0;
		if (by_choice != 0) {
			preferred = by_choice;
			first_source = csrc;
			snprintf(first_setting, sizeof(first_setting), "%s", choice);
		} else {
			preferred = query_configured(xdev->get_property, xdev);
			if (preferred != U_RENDERING_MODES_NO_PREFERENCE) {
				first_source = XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT;
			}
		}
	}

	uint32_t steer_inout = steer;
	struct u_rendering_modes_trim_info info;
	const enum u_rendering_modes_trim_result res =
	    u_rendering_modes_trim(xdev->rendering_modes, &xdev->rendering_mode_count, &steer_inout, preferred, &info);

	if (res == U_RENDERING_MODES_TRIM_REFUSED) {
		U_LOG_W(
		    "Display driver '%s' declares %u rendering modes, but %s - the table is kept as it is and does NOT "
		    "meet the two-slot contract (ADR-065: [0] = 2D, [1] = 3D)",
		    name, before_count,
		    !u_rendering_mode_is_flat(&xdev->rendering_modes[0]) ? "its mode 0 is not the 1-view 2D mode"
		                                                         : "none of its modes is a 3D mode");
		return res;
	}

	// The active index the runtime serves.
	if (!again) {
		xdev->hmd->active_rendering_mode_index = steer_inout; // translated by the selection
	} else if (before_active >= U_RENDERING_MODES_RUNTIME_COUNT) {
		// The driver's own rewrite also wrote its own active index.
		xdev->hmd->active_rendering_mode_index =
		    before_active < XRT_MAX_RENDERING_MODES ? info.from_driver[before_active] : U_RENDERING_MODE_2D;
	} // else: already a run-time index (0 or 1): it keeps its meaning.

	// The device's max view count is the ENVELOPE's (ADR-066 C1.4): it is
	// what PRIMARY_MULTIVIEW_DXR reports and what the IPC proxy copies, and it
	// must not change when a config switch remaps slot 1.
	const uint32_t max_views = u_rendering_modes_max_view_count(xdev->rendering_modes, info.envelope_count);
	if (max_views > 0 && max_views <= XRT_MAX_VIEWS) {
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
		snprintf(rec->name, sizeof(rec->name), "%s", name != NULL ? name : "?");
		if (!again) {
			rec->config_source = first_source;
			snprintf(rec->setting, sizeof(rec->setting), "%s", first_setting);
		}
		record_unlock(rec);
		// Installed at most once per device, whatever slot 1 maps to: a
		// later remap needs the translation even when today's is identity.
		(void)record_wrap(rec, xdev);
		wrapped = rec->wrapped;
	} else {
		no_room = true;
	}

	log_trim(name, before_count, before_active, xdev, &info, wrapped, no_room, again);
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


/*
 *
 * ADR-066 C1: the envelope and live config switches.
 *
 */

uint32_t
u_device_rendering_modes_envelope_count(struct xrt_device *xdev)
{
	if (xdev == NULL) {
		return 0;
	}
	uint32_t n = xdev->rendering_mode_count;
	struct u_rm_record *r = record_find(xdev);
	if (r != NULL && r->envelope_count > n) {
		n = r->envelope_count;
	}
	return n < XRT_MAX_RENDERING_MODES ? n : XRT_MAX_RENDERING_MODES;
}

uint32_t
u_device_rendering_modes_generation(struct xrt_device *xdev)
{
	struct u_rm_record *r = (xdev != NULL) ? record_find(xdev) : NULL;
	return r != NULL ? r->generation : 0;
}

bool
u_device_rendering_modes_refresh_config(struct xrt_device *xdev, const char *driver_name)
{
	if (xdev == NULL || xdev->hmd == NULL) {
		return false;
	}
	struct u_rm_record *r = record_find(xdev);
	if (r == NULL || !r->wrapped || r->get_property == NULL ||
	    r->envelope_count < U_RENDERING_MODES_RUNTIME_COUNT || r->driver_count <= U_RENDERING_MODES_RUNTIME_COUNT) {
		return false; // a two-mode driver: nothing to remap
	}

	// The per-screen choice first (ADR-066 C1: the user's "3D mode" for this
	// screen), outside our lock: the resolver reads the store.
	uint32_t csrc = 0;
	char choice[32];
	const bool have_choice = resolve_choice(xdev, choice, sizeof(choice), &csrc);

	record_lock(r);
	const uint32_t cur = r->kept_driver_index;
	int32_t want = -1;
	uint32_t source = XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	bool refused_choice = false;
	if (have_choice) {
		const uint32_t j = find_choice(xdev->rendering_modes, r->envelope_count, cur, choice);
		if (j != 0) {
			want = (int32_t)j;
			source = csrc;
			r->last_refused_choice[0] = '\0';
		} else if (strcmp(r->last_refused_choice, choice) != 0) {
			snprintf(r->last_refused_choice, sizeof(r->last_refused_choice), "%s", choice);
			refused_choice = true;
		}
	}
	if (want < 0) {
		// The driver's own default (sim-display: its OUTPUT setting).
		const uint32_t served = driver_view_begin(r, xdev);
		const xrt_result_t ret = r->get_property(xdev, XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE, &want);
		driver_view_end(r, xdev, served);
		if (ret == XRT_SUCCESS && want >= 1) {
			source = XRT_STATUS_MODE_3D_SOURCE_DRIVER_DEFAULT;
		} else {
			want = -1;
		}
	}
	r->config_source = (want < 0) ? r->config_source : source;
	if (want >= 0) {
		snprintf(r->setting, sizeof(r->setting), "%s",
		         (source == XRT_STATUS_MODE_3D_SOURCE_USER || source == XRT_STATUS_MODE_3D_SOURCE_MACHINE)
		             ? choice
		             : "");
	}

	if (refused_choice) {
		U_LOG_W(
		    "Display driver '%s': the screen's 3D mode '%s' names no 3D entry of the %u it declared at "
		    "start-up "
		    "- using the driver's default (ADR-066 C1)",
		    driver_name != NULL ? driver_name : r->name, choice, r->driver_count);
	}
	if (want < 0 || (uint32_t)want == cur) {
		r->last_refused = -1;
		record_unlock(r);
		return false;
	}

	char was[XRT_DEVICE_NAME_LEN];
	snprintf(was, sizeof(was), "%s", xdev->rendering_modes[1].mode_name);
	struct u_rendering_modes_trim_info info;
	if (!u_rendering_modes_remap(xdev->rendering_modes, r->envelope_count, cur, (uint32_t)want, &info)) {
		const bool first = r->last_refused != want;
		r->last_refused = want;
		record_unlock(r);
		if (first) {
			U_LOG_W(
			    "Display driver '%s': its configuration names mode %d, which is not a 3D entry of the %u "
			    "it "
			    "declared at start-up - slot 1 stays '%s' (ADR-066 C1: an entry outside the declared list "
			    "applies at the next start)",
			    driver_name != NULL ? driver_name : r->name, want, r->driver_count, was);
		}
		return false;
	}
	info.original_count = r->driver_count; // the driver still lists every entry
	u_rendering_modes_fill_maps_listed(xdev->rendering_modes, r->driver_count, r->kept_driver_index, (uint32_t)want,
	                                   &info);
	record_set_maps(r, &info);
	r->last_refused = -1;
	r->generation++;
	const uint32_t gen = r->generation;
	const uint32_t active = xdev->hmd->active_rendering_mode_index;
	record_unlock(r);

	U_LOG_W(
	    "Display driver '%s': config switch applied live (ADR-066 C1) - slot 1 is now '%s' (the driver's mode %d, "
	    "%u views, %ux%u tiles; was '%s'). Active mode stays %u; table generation %u.",
	    driver_name != NULL ? driver_name : r->name, xdev->rendering_modes[1].mode_name, want,
	    xdev->rendering_modes[1].view_count, xdev->rendering_modes[1].tile_columns,
	    xdev->rendering_modes[1].tile_rows, was, active, gen);

	// The device is showing slot 1: the driver (and its display processor)
	// must draw the new entry now. Through the wrapper, so it arrives in the
	// driver's numbering.
	if (active == U_RENDERING_MODE_3D) {
		xrt_device_set_property(xdev, XRT_DEVICE_PROPERTY_OUTPUT_MODE, (int32_t)U_RENDERING_MODE_3D);
	}
	return true;
}

uint32_t
u_device_rendering_modes_sync(struct xrt_device *xdev, uint64_t now_ns)
{
	struct u_rm_record *r = (xdev != NULL) ? record_find(xdev) : NULL;
	if (r == NULL) {
		return 0;
	}
	if (r->sync_fn != NULL) {
		return r->sync_fn(xdev);
	}
	// ADR-047 Amendment 10: a per-user store write (`dp mode set`) is honoured
	// at the next poll, not after the rest of the second (one stat).
	const uint64_t stamp = r->wrapped ? u_setting_user_stamp() : 0;
	if (r->wrapped &&
	    (r->last_poll_ns == 0 || now_ns - r->last_poll_ns >= U_RM_CONFIG_POLL_NS || stamp != r->store_stamp)) {
		r->last_poll_ns = now_ns;
		r->store_stamp = stamp;
		(void)u_device_rendering_modes_refresh_config(xdev, NULL);
	}
	return r->generation;
}

bool
u_device_rendering_modes_promote_choice(struct xrt_device *xdev, const char *choice)
{
	struct u_rm_record *r = (xdev != NULL) ? record_find(xdev) : NULL;
	if (r == NULL || !r->wrapped || choice == NULL || choice[0] == '\0') {
		return false;
	}
	bool promoted = false;
	record_lock(r);
	const uint32_t n = r->driver_count < XRT_MAX_RENDERING_MODES ? r->driver_count : XRT_MAX_RENDERING_MODES;
	for (uint32_t j = 1; j < n && !promoted; j++) {
		struct xrt_rendering_mode *m = &xdev->rendering_modes[pos_of(j, r->kept_driver_index)];
		if ((m->mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) == 0 || !u_rendering_mode_is_3d(m) ||
		    m->view_count > XRT_MAX_VIEWS || !u_rendering_mode_matches_choice(m, choice)) {
			continue;
		}
		m->mode_flags &= ~XRT_RENDERING_MODE_FLAG_START_UP_ONLY;
		r->envelope_count = u_rendering_modes_envelope_of(xdev->rendering_modes, n);
		if (xdev->hmd != NULL) {
			const uint32_t mv = u_rendering_modes_max_view_count(xdev->rendering_modes, r->envelope_count);
			if (mv > xdev->hmd->view_count) {
				xdev->hmd->view_count = mv;
			}
		}
		U_LOG_W(
		    "Display driver '%s': the screen's stored 3D mode '%s' is declared only at start-up - declared "
		    "for this instance (%u views; envelope %u entries, located view count %u)",
		    r->name, m->mode_name, m->view_count, r->envelope_count,
		    xdev->hmd != NULL ? xdev->hmd->view_count : 0u);
		promoted = true;
	}
	record_unlock(r);
	return promoted;
}

void
u_rendering_modes_set_choice_resolver(u_rendering_modes_choice_fn fn)
{
	g_choice_resolver = fn;
}

void
u_device_rendering_modes_fill_status(struct xrt_device *xdev, struct xrt_status_mode_3d *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (xdev == NULL || xdev->rendering_mode_count < U_RENDERING_MODES_RUNTIME_COUNT) {
		return;
	}
	struct u_rm_record *r = record_find(xdev);
	if (r != NULL) {
		record_lock(r);
	}
	const uint32_t env = u_device_rendering_modes_envelope_count(xdev);
	const uint32_t n = (r != NULL && r->wrapped && r->driver_count > env) ? r->driver_count : env;
	const uint32_t kept = (r != NULL && r->wrapped) ? r->kept_driver_index : 1u;
	out->valid = true;
	u_rendering_mode_slug(xdev->rendering_modes[1].mode_name, out->active, sizeof(out->active));
	out->source = r != NULL ? r->config_source : XRT_STATUS_MODE_3D_SOURCE_RUNTIME_DEFAULT;
	if (r != NULL) {
		snprintf(out->setting, sizeof(out->setting), "%s", r->setting);
	}
	for (uint32_t j = 1; j < n && out->choice_count < XRT_STATUS_MAX_MODE_3D_CHOICES; j++) {
		const struct xrt_rendering_mode *m = &xdev->rendering_modes[pos_of(j, kept)];
		if (!u_rendering_mode_is_3d(m) && !(n == U_RENDERING_MODES_RUNTIME_COUNT && j == 1)) {
			continue;
		}
		struct xrt_status_mode_3d_choice *c = &out->choices[out->choice_count++];
		u_rendering_mode_slug(m->mode_name, c->name, sizeof(c->name));
		snprintf(c->label, sizeof(c->label), "%s", m->mode_name);
		c->views = m->view_count;
		c->tracked = (m->mode_flags & XRT_RENDERING_MODE_FLAG_HAS_TRACKING) != 0;
		c->passthrough = u_rendering_mode_is_passthrough_3d(m);
		// ADR-066 C1.3: the runtime computes `applies` / `reason` itself.
		if (m->view_count > XRT_MAX_VIEWS) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOT_SUPPORTED;
			snprintf(c->reason, sizeof(c->reason), "needs %u views; at most %u can be presented",
			         m->view_count, (unsigned)XRT_MAX_VIEWS);
		} else if (j >= env || (m->mode_flags & XRT_RENDERING_MODE_FLAG_START_UP_ONLY) != 0) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_RESTART_APP;
			snprintf(c->reason, sizeof(c->reason), "declared only at start-up: restart the app to apply");
		} else if (m->view_count > 2) {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOW;
			snprintf(c->reason, sizeof(c->reason), "%u views: stereo apps show 2D", m->view_count);
		} else {
			c->applies = XRT_STATUS_MODE_3D_APPLIES_NOW;
		}
	}
	if (r != NULL) {
		record_unlock(r);
	}
}

void
u_device_rendering_modes_publish(struct xrt_device *xdev, uint32_t envelope_count, uint32_t generation)
{
	if (xdev == NULL) {
		return;
	}
	struct u_rm_record *r = record_find(xdev);
	if (r == NULL) {
		r = record_alloc(xdev);
	}
	if (r == NULL || r->wrapped) {
		return; // a selected in-process device owns its record
	}
	r->driver_count = envelope_count <= XRT_MAX_RENDERING_MODES ? envelope_count : XRT_MAX_RENDERING_MODES;
	r->envelope_count = r->driver_count;
	r->generation = generation;
}

void
u_device_rendering_modes_set_sync_hook(struct xrt_device *xdev, u_rendering_modes_sync_fn fn)
{
	struct u_rm_record *r = (xdev != NULL) ? record_find(xdev) : NULL;
	if (r == NULL && xdev != NULL && fn != NULL) {
		r = record_alloc(xdev);
	}
	if (r != NULL && !r->wrapped) {
		r->sync_fn = fn;
	}
}

void
u_device_rendering_modes_forget(struct xrt_device *xdev)
{
	struct u_rm_record *r = (xdev != NULL) ? record_find(xdev) : NULL;
	if (r != NULL && !r->wrapped) {
		record_clear(r);
	}
}

void
u_rendering_modes_log_key_refused(const struct xrt_rendering_mode *mode, uint32_t fill_views)
{
	// One INFO line per refusal burst, never per press: a held or hammered V
	// key must not flood the log (debug-logging.md).
	static int64_t s_last_ns = 0;
	const int64_t now_ns = (int64_t)os_monotonic_get_ns();
	if (s_last_ns != 0 && now_ns - s_last_ns < 2 * (int64_t)1000000000) {
		return;
	}
	s_last_ns = now_ns;
	U_LOG_I(
	    "Mode key ignored (#1499, ADR-065): mode 1 '%s' has %u views and this session can fill %u - it stays "
	    "in 2D (begin PRIMARY_MULTIVIEW_DXR to use this kind)",
	    mode != NULL ? mode->mode_name : "?", mode != NULL ? mode->view_count : 0, fill_views);
}
