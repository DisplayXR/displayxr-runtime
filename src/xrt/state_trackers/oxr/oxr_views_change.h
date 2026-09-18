// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1488: the XR_EXT_view_configuration_views_change state machine.
 *
 * Holds the shadow copy of @ref oxr_system::views whose
 * recommendedImageRect{Width,Height} are allowed to move, plus the
 * spec-mandated 1 Hz doorbell throttle.
 *
 * Deliberately a standalone translation unit with no dependency on
 * oxr_objects.h: the unit test (tests/tests_oxr_view_config_views_change.cpp)
 * compiles this .c straight into the test binary and drives it with an
 * injected @p now_ns, so the throttle is testable without sleeping, without a
 * compositor and without a wall clock. See the file header of that test.
 *
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_limits.h"

#include "os/os_threading.h"

#include <openxr/openxr.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The spec's rate limit: "The runtime must: not use this event for frequent
 * (at a rate faster than 1Hz per view configuration) adjustments of the
 * resolution." Not a tuning knob.
 */
#define OXR_VIEWS_CHANGE_MIN_PERIOD_NS (1000 * 1000 * 1000ULL)

/*!
 * Live-view-size state. Embedded by value in @ref oxr_system.
 *
 * ONE instance == ONE view configuration: the spec's rate limit is worded "per
 * view configuration", so a runtime that later advertises more than one view
 * configuration type holds one of these per type, with no API change here -
 * every entry point below takes the count (and the caller the type) explicitly.
 *
 * @ref lock guards @ref views_live, @ref valid, @ref last_w / @ref last_h,
 * @ref last_push_ns and @ref pending_push. It is a DEDICATED mutex on purpose:
 * oxr_instance::event::mutex is already held by the event push, and
 * oxr_system::sync_actions_mutex is trylock-probed by the GET_XDEV_BY_ROLE
 * macros — giving either a second purpose invites a deadlock (#1488 R3).
 */
struct oxr_views_change
{
	//! Shadow of oxr_system::views. Only the two recommended fields ever move.
	XrViewConfigurationView views_live[XRT_MAX_VIEWS];

	//! True once a real size change has been written into @ref views_live.
	bool valid;

	//! Last dims seen by @ref oxr_views_change_update, for edge detection. 0 = no sample yet.
	uint32_t last_w, last_h;

	//! Monotonic ns of the last emitted doorbell, for the 1 Hz throttle.
	uint64_t last_push_ns;

	//! A change was written but its doorbell was throttled; fire it when the window closes.
	bool pending_push;

	struct os_mutex lock;
};

/*!
 * Zero the state and create @ref oxr_views_change::lock. Returns < 0 on failure.
 */
int
oxr_views_change_init(struct oxr_views_change *vc);

/*!
 * Destroy @ref oxr_views_change::lock.
 */
void
oxr_views_change_fini(struct oxr_views_change *vc);

/*!
 * Seed the shadow from the frozen xrCreateInstance-time snapshot. Leaves
 * @ref oxr_views_change::valid false, so the read path keeps answering with the
 * frozen array until a real change lands.
 */
void
oxr_views_change_seed(struct oxr_views_change *vc, const XrViewConfigurationView *frozen, uint32_t view_count);

/*!
 * Feed the just-computed per-view render dims.
 *
 * On a real change (and only then) rewrites @ref oxr_views_change::views_live
 * from @p base with @p w / @p h substituted into recommendedImageRect{Width,
 * Height} — maxImageRect* and both sample counts are copied verbatim and never
 * assigned, so ADR-010's worst-case invariant and the extension's
 * "recommended values only" must: are structural rather than conventional.
 *
 * @param ext_enabled The calling session's instance has
 *        XR_EXT_view_configuration_views_change enabled AND the
 *        DXR_VIEWS_CHANGE_LIVE / DXR_VIEWS_CHANGE_EVENT kill switches allow it.
 *        When false this returns immediately: no lock, no copy, no event, so a
 *        non-EXT app pays nothing beyond the pre-existing code.
 * @param now_ns Monotonic nanoseconds; injected so the throttle is testable.
 *
 * @return true iff the caller should push XrEventDataViewConfigurationViewsChangedEXT.
 */
bool
oxr_views_change_update(struct oxr_views_change *vc,
                        const XrViewConfigurationView *base,
                        uint32_t view_count,
                        uint32_t w,
                        uint32_t h,
                        uint64_t now_ns,
                        bool ext_enabled);

/*!
 * Pick the array xrEnumerateViewConfigurationViews must answer from.
 *
 * @return @p frozen (the xrCreateInstance-time snapshot) unless the extension is
 *         enabled, the DXR_VIEWS_CHANGE_LIVE kill switch is on, and a change has
 *         actually landed — in which case @p scratch, filled with exactly
 *         @p count entries - the count of the REQUESTED view configuration type,
 *         never a baked-in sys->view_count.
 */
const XrViewConfigurationView *
oxr_views_change_select(struct oxr_views_change *vc,
                        const XrViewConfigurationView *frozen,
                        uint32_t count,
                        bool ext_enabled,
                        bool live_enabled,
                        XrViewConfigurationView *scratch);

#ifdef __cplusplus
}
#endif
