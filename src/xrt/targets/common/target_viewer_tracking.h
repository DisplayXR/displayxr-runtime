// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Viewer arbitration + composed tracking advertisement (ADR-054 §4.1, N6 Part 2).
 *
 * Two halves:
 *
 *  - **Pure policy** (@ref target_viewer_arbitrate,
 *    @ref target_viewer_compose_advertisement) — no I/O, pinned by
 *    `tests/tests_viewer_arbitration.cpp`. Per screen, in order:
 *      (a) the display processor's own eyes when it tracks — even while its
 *          tracker has lost the face; never a second source spliced in;
 *      (b) the best enabled viewer-tracking provider (`PreferredPlugin`, then
 *          ProbeOrder) — only when the display's viewer policy is EXTERNAL_OK;
 *      (c) the nominal viewer.
 *    The advertisement `XR_DXR_display_info` reports is composed from the
 *    answer: advertised = DP tracks OR (EXTERNAL_OK AND a provider with a
 *    usable source at system creation). Static per instance; live state rides
 *    `is_tracking` and `XrEventDataEyeTrackingStateChangedDXR`.
 *
 *  - **Glue** — runs the arbitration for the primary screen when the system's
 *    display info is applied (`target_instance.c`), composes the advertisement
 *    into the system compositor info and the head's rendering modes, starts the
 *    chosen provider on demand, keeps its samples in an eye-pair ring and
 *    exposes the result as @ref xrt_viewer_tracking on the system devices.
 *
 * Hosting in this release: in-process only. The service process
 * (@ref target_viewer_tracking_set_service_process) never loads a provider —
 * its IPC clients' views are computed in the service from the DP, and the
 * provider is not spliced there yet (follow-up). It therefore never composes
 * the advertisement either, so an IPC client is never told about tracking the
 * service would not deliver.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_viewer_tracking.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_plugin_iface;
struct xrt_plugin_instance;
struct xrt_plugin_display_info;
struct xrt_system_compositor_info;

//! What the arbitration of ONE screen is fed.
struct target_viewer_inputs
{
	bool dp_tracks;          //!< the DP advertises eye tracking (`supported_eye_tracking_modes != 0`)
	uint32_t dp_policy;      //!< enum xrt_plugin_viewer_policy (absent slot already mapped to FIXED)
	bool hosted;             //!< this process may host providers (false in the service, for now)
	bool provider_usable;    //!< a resident provider claimed a usable source
	const char *provider_id; //!< its registration id (when @ref provider_usable)
};

//! The answer for one screen.
struct target_viewer_decision
{
	enum xrt_viewer_source source;
	char provider_id[64]; //!< SOURCE_PROVIDER: the provider row; "" otherwise
	//! One line: why this source, and — when a provider exists but is not used —
	//! why it is in standby ("display tracks the viewer on this screen",
	//! "display is FIXED", "no source", ...). "" when nothing needs saying.
	char reason[128];
	uint32_t dp_policy; //!< echo of the input, for status
};

//! `XR_DXR_display_info` eye-tracking mode bits (bit 0 MANAGED, bit 1 MANUAL).
#define TARGET_VIEWER_MODE_MANAGED_BIT 1u
#define TARGET_VIEWER_MODE_MANUAL_BIT 2u

//! The pure per-screen rule (a) / (b) / (c). Never NULL in, always fills @p out.
void
target_viewer_arbitrate(const struct target_viewer_inputs *in, struct target_viewer_decision *out);

/*!
 * The composed advertisement. DP tracks → its own caps unchanged. Provider
 * chosen → MANAGED|MANUAL supported, MANAGED default, and @p out_mark_modes
 * true (the caller sets HAS_TRACKING on the display's 3D modes). Otherwise the
 * DP's (honest, zero) caps.
 */
void
target_viewer_compose_advertisement(uint32_t dp_supported,
                                    uint32_t dp_default,
                                    const struct target_viewer_decision *d,
                                    uint32_t *out_supported,
                                    uint32_t *out_default,
                                    bool *out_mark_modes);

//! "dp" / "provider" / "nominal".
const char *
target_viewer_source_str(enum xrt_viewer_source s);

//! "OWN" / "EXTERNAL_OK" / "FIXED".
const char *
target_viewer_policy_str(uint32_t policy);

//! The status spelling: "dp" / "provider:<id>" / "nominal".
void
target_viewer_source_label(const struct target_viewer_decision *d, char *out, size_t cap);


/*
 *
 * Glue.
 *
 */

//! The service calls this once, before creating its instance: never host providers here (see file comment).
void
target_viewer_tracking_set_service_process(bool service);

/*!
 * Evaluate the primary screen without starting anything: the DP's policy
 * (through @p plugin's slot), whether it tracks (@p dp_supported), and — only
 * when that leaves room for one — the tracking class scan. What the CLI's
 * self-test and the status snapshot report. Pure but for the one-shot scan.
 */
void
target_viewer_tracking_evaluate(const struct xrt_plugin_iface *plugin,
                                struct xrt_plugin_instance *inst,
                                uint32_t dp_supported,
                                struct target_viewer_decision *out);

/*!
 * Called by `apply_plugin_display_info` after it copied the DP's answer into
 * @p info: evaluate, record the decision for this process, compose the
 * advertisement into @p info and @p head's mode flags, publish the display
 * geometry to providers. Idempotent across refreshes.
 */
void
target_viewer_tracking_apply(struct xrt_system_compositor_info *info,
                             struct xrt_device *head,
                             const struct xrt_plugin_iface *plugin,
                             const struct xrt_plugin_display_info *pdi);

//! The decision recorded by the last apply (false when none ran in this process).
bool
target_viewer_tracking_get_decision(struct target_viewer_decision *out);

//! The interface for @ref xrt_system_devices::viewer_tracking. Never NULL.
struct xrt_viewer_tracking *
target_viewer_tracking_get_iface(void);

//! Is the chosen provider started, and what does it say about itself (`get_status`)? For status.
bool
target_viewer_tracking_provider_running(uint32_t *out_status);

//! Stop the provider (system teardown). A later system starts it again on demand.
void
target_viewer_tracking_shutdown(void);

#ifdef __cplusplus
}
#endif
