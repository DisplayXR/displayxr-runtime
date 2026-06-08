// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Synthetic-input backend for XR_EXT_conformance_automation.
 *
 * The Khronos OpenXR Conformance Test Suite (CTS) injects controller state
 * through this extension so it can drive the action/input pipeline without a
 * human pressing buttons. We store the injected state per @ref oxr_session and
 * apply it inside xrSyncActions / the action-space locate path.
 *
 * Design: overrides are keyed by `source_path`, which is the suggested-binding
 * path the CTS passes as `inputSourcePath`. That same XrPath is stored on each
 * resolved input as @ref oxr_action_input::bound_path, so the apply side is a
 * direct XrPath compare — no reverse mapping from path to a device input index.
 *
 * Lifetime: all state lives in @ref oxr_session::conformance and is freed with
 * the session (see @ref oxr_conformance_teardown). Nothing is process-global —
 * the CTS creates and destroys the runtime hundreds of times per run.
 *
 * @ingroup oxr_main
 */

#include "oxr_objects.h"
#include "oxr_logger.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include <string.h>

/*
 *
 * Helpers.
 *
 */

//! Is the conformance extension enabled and the state ready to use?
static bool
conformance_enabled(struct oxr_session *sess)
{
	return sess != NULL && sess->sys->inst->extensions.EXT_conformance_automation && sess->conformance.inited;
}

//! Caller must hold the conformance mutex.
static bool
device_is_active(struct oxr_conformance_state *c, XrPath top_level_path)
{
	for (size_t i = 0; i < c->active_device_count; i++) {
		if (c->active_devices[i] == top_level_path) {
			return true;
		}
	}
	return false;
}

//! Caller must hold the conformance mutex. Finds an existing override for
//! @p source_path, or the first free slot, or NULL if the table is full.
static struct oxr_conformance_override *
find_or_alloc_override(struct oxr_conformance_state *c, XrPath source_path)
{
	struct oxr_conformance_override *free_slot = NULL;
	for (size_t i = 0; i < OXR_MAX_CONFORMANCE_OVERRIDES; i++) {
		struct oxr_conformance_override *o = &c->overrides[i];
		if (o->used && o->source_path == source_path) {
			return o;
		}
		if (!o->used && free_slot == NULL) {
			free_slot = o;
		}
	}
	return free_slot;
}


/*
 *
 * Lifecycle.
 *
 */

XrResult
oxr_conformance_init(struct oxr_logger *log, struct oxr_session *sess)
{
	struct oxr_conformance_state *c = &sess->conformance;
	if (c->inited) {
		return XR_SUCCESS;
	}

	int ret = os_mutex_init(&c->mutex);
	if (ret != 0) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
		                 "Failed to init conformance-automation mutex (%i)", ret);
	}

	c->active_device_count = 0;
	memset(c->overrides, 0, sizeof(c->overrides));
	c->inited = true;
	return XR_SUCCESS;
}

void
oxr_conformance_teardown(struct oxr_session *sess)
{
	struct oxr_conformance_state *c = &sess->conformance;
	if (!c->inited) {
		return;
	}
	os_mutex_destroy(&c->mutex);
	c->inited = false;
	c->active_device_count = 0;
	memset(c->overrides, 0, sizeof(c->overrides));
}


/*
 *
 * Setters (called from the API entry points).
 *
 */

XrResult
oxr_conformance_set_device_active(
    struct oxr_logger *log, struct oxr_session *sess, XrPath top_level_path, bool active)
{
	XrResult res = oxr_conformance_init(log, sess);
	if (res != XR_SUCCESS) {
		return res;
	}
	struct oxr_conformance_state *c = &sess->conformance;

	os_mutex_lock(&c->mutex);

	bool present = device_is_active(c, top_level_path);
	if (active && !present) {
		if (c->active_device_count >= OXR_MAX_CONFORMANCE_DEVICES) {
			os_mutex_unlock(&c->mutex);
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
			                 "conformance-automation active-device table full");
		}
		c->active_devices[c->active_device_count++] = top_level_path;
	} else if (!active && present) {
		// Compact the array, preserving order is unimportant.
		for (size_t i = 0; i < c->active_device_count; i++) {
			if (c->active_devices[i] == top_level_path) {
				c->active_devices[i] = c->active_devices[c->active_device_count - 1];
				c->active_device_count--;
				break;
			}
		}
	}

	os_mutex_unlock(&c->mutex);
	return XR_SUCCESS;
}

XrResult
oxr_conformance_set_input_value(struct oxr_logger *log,
                                struct oxr_session *sess,
                                XrPath top_level_path,
                                XrPath source_path,
                                enum xrt_input_type type,
                                union xrt_input_value value)
{
	XrResult res = oxr_conformance_init(log, sess);
	if (res != XR_SUCCESS) {
		return res;
	}
	struct oxr_conformance_state *c = &sess->conformance;

	os_mutex_lock(&c->mutex);

	struct oxr_conformance_override *o = find_or_alloc_override(c, source_path);
	if (o == NULL) {
		os_mutex_unlock(&c->mutex);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "conformance-automation override table full");
	}

	o->used = true;
	o->top_level_path = top_level_path;
	o->source_path = source_path;
	o->type = type;
	o->value = value;
	o->is_pose = false;
	// Monotonic-ish timestamp so lastChangeTime advances on each set.
	o->timestamp = os_monotonic_get_ns();

	os_mutex_unlock(&c->mutex);
	return XR_SUCCESS;
}

XrResult
oxr_conformance_set_input_pose(struct oxr_logger *log,
                               struct oxr_session *sess,
                               XrPath top_level_path,
                               XrPath source_path,
                               struct xrt_pose pose)
{
	XrResult res = oxr_conformance_init(log, sess);
	if (res != XR_SUCCESS) {
		return res;
	}
	struct oxr_conformance_state *c = &sess->conformance;

	os_mutex_lock(&c->mutex);

	struct oxr_conformance_override *o = find_or_alloc_override(c, source_path);
	if (o == NULL) {
		os_mutex_unlock(&c->mutex);
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "conformance-automation override table full");
	}

	o->used = true;
	o->top_level_path = top_level_path;
	o->source_path = source_path;
	o->is_pose = true;
	o->pose.pose = pose;
	o->pose.relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                         XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
	o->timestamp = os_monotonic_get_ns();

	os_mutex_unlock(&c->mutex);
	return XR_SUCCESS;
}


/*
 *
 * Lookups (called from the xrSyncActions / locate hot paths).
 *
 */

bool
oxr_conformance_lookup_value(struct oxr_session *sess,
                             XrPath source_path,
                             union xrt_input_value *out_value,
                             int64_t *out_timestamp)
{
	if (!conformance_enabled(sess) || source_path == XR_NULL_PATH) {
		return false;
	}
	struct oxr_conformance_state *c = &sess->conformance;

	bool found = false;
	os_mutex_lock(&c->mutex);
	for (size_t i = 0; i < OXR_MAX_CONFORMANCE_OVERRIDES; i++) {
		struct oxr_conformance_override *o = &c->overrides[i];
		if (!o->used || o->is_pose || o->source_path != source_path) {
			continue;
		}
		if (!device_is_active(c, o->top_level_path)) {
			break; // Override exists but its device is inactive; suppress.
		}
		*out_value = o->value;
		*out_timestamp = o->timestamp;
		found = true;
		break;
	}
	os_mutex_unlock(&c->mutex);
	return found;
}

bool
oxr_conformance_lookup_pose(struct oxr_session *sess, XrPath source_path, struct xrt_space_relation *out_relation)
{
	if (!conformance_enabled(sess) || source_path == XR_NULL_PATH) {
		return false;
	}
	struct oxr_conformance_state *c = &sess->conformance;

	bool found = false;
	os_mutex_lock(&c->mutex);
	for (size_t i = 0; i < OXR_MAX_CONFORMANCE_OVERRIDES; i++) {
		struct oxr_conformance_override *o = &c->overrides[i];
		if (!o->used || !o->is_pose || o->source_path != source_path) {
			continue;
		}
		if (!device_is_active(c, o->top_level_path)) {
			break;
		}
		*out_relation = o->pose;
		found = true;
		break;
	}
	os_mutex_unlock(&c->mutex);
	return found;
}
