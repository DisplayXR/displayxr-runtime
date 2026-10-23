// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The webcam tracker core's one dependency on its host: a settings
 *         callback, `get(key) -> string`.
 *
 * The core (camera worker, face estimator, geometry) is compiled into two
 * hosts and reads every knob through this interface, never through a host's
 * own settings shim and never through `getenv`:
 *  - sim_tracking (`SOURCE=webcam`) satisfies it from its host iface's
 *    `get_setting` (`DXR_TRACKING_SIM_TRACKING_<KEY>`, the tracking store);
 *  - sim_display satisfies it from its settings shim, for the one-release
 *    `WEBCAM_TRACKING` deprecation alias (`DXR_DP_SIM_DISPLAY_<KEY>`, the DP
 *    store, and the legacy `SIM_DISPLAY_<KEY>` environment names).
 *
 * Both hosts use the SAME key names (`WEBCAM_DEVICE`, `WEBCAM_MODEL`, ...),
 * so a value means the same thing whichever host reads it.
 *
 * @ingroup drv_sim_tracking
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sim_webcam_settings
{
	/*!
	 * The value of @p key into @p buf (@p cap bytes, NUL-terminated), or
	 * false with `buf[0] = 0` when nothing sets it. Thread-safe; may be
	 * called from the tracker's worker thread.
	 */
	bool (*get)(void *ctx, const char *key, char *buf, size_t cap);
	void *ctx;
};

//! @p key through @p s; false (and `buf[0] = 0`) when @p s is NULL or nothing sets it.
static inline bool
sim_webcam_setting(const struct sim_webcam_settings *s, const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	if (s == NULL || s->get == NULL || !s->get(s->ctx, key, buf, cap)) {
		buf[0] = '\0';
		return false;
	}
	return buf[0] != '\0';
}

//! Is @p key set to something truthy? Unset / empty / "0" / "false" / "no" / "off" are off.
static inline bool
sim_webcam_setting_on(const struct sim_webcam_settings *s, const char *key)
{
	char v[16];
	if (!sim_webcam_setting(s, key, v, sizeof(v))) {
		return false;
	}
	return !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "off") == 0);
}

#ifdef __cplusplus
}
#endif
