// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Viewer arbitration + composed tracking advertisement (ADR-054 §4.1, N6 Part 2).
 *
 * Three parts:
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
 *  - **Per screen** (@ref target_viewer_arbitrate_screens, pure too): the
 *    same rule for every screen, each read from the display processor bound
 *    to THAT screen (its segment DP: the plug-in that won the monitor, its
 *    `get_viewer_policy` for that monitor, whether it tracks). One provider
 *    serves every EXTERNAL_OK screen without a tracking display; a tracking
 *    display keeps its own screen; FIXED stays nominal. The provider reports
 *    in the PRIMARY screen's display space (the geometry the host hands it,
 *    the screen it is calibrated to); another screen gets the sample
 *    translated by the desktop-geometry offset between the two screen
 *    centres (@ref target_viewer_map_offset — the workspace's one-layout
 *    model, ADR-052 D2: coplanar screens).
 *
 *  - **Glue** — runs the arbitration for the primary screen when the system's
 *    display info is applied (`target_instance.c`), composes the advertisement
 *    into the system compositor info and the head's rendering modes, re-runs
 *    the per-screen arbitration every time the display topology is published
 *    (system create, hotplug, re-probe), starts the chosen provider on
 *    demand, keeps its samples in an eye-pair ring and exposes the result as
 *    @ref xrt_viewer_tracking on the system devices.
 *
 * Hosting: whichever process computes the views. An in-process runtime hosts
 * the provider for its own session; the service hosts it ONCE for every IPC
 * client (its view path splices the provider's eyes, and the composed
 * advertisement rides `system_compositor_get_info` to the clients). An IPC
 * client never creates a native system, so it never loads a provider: one
 * provider per process that computes views, never two for one view path.
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
struct xrt_screen_list;
struct xrt_dp_factory_registry;

//! What the arbitration of ONE screen is fed.
struct target_viewer_inputs
{
	bool dp_tracks;          //!< the DP advertises eye tracking (`supported_eye_tracking_modes != 0`)
	uint32_t dp_policy;      //!< enum xrt_plugin_viewer_policy (absent slot already mapped to FIXED)
	bool hosted;             //!< this process may host providers
	bool provider_usable;    //!< a resident provider claimed a usable source
	const char *provider_id; //!< its registration id (when @ref provider_usable)
	//! An enabled provider row is registered (loaded or not: a tracking display never loads one).
	bool provider_enabled;
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
	bool dp_tracks;     //!< echo of the input, for status
	//! A provider could serve here but for the display: usable, or (when the
	//! display tracks and so no scan ran) an enabled row — a DP / OWN answer is then "standby".
	bool provider_available;
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
 * Per screen (pure).
 *
 */

//! Max screens the per-screen arbitration keeps (= XRT_SCREEN_LIST_MAX).
#define TARGET_VIEWER_MAX_SCREENS 16

//! One screen as the per-screen arbitration sees it (read from that screen's own DP).
struct target_viewer_screen_in
{
	uint64_t screen_id; //!< registry monitor id
	bool is_primary;    //!< the system-default screen: the session DP's panel, the provider's home
	bool has_dp;        //!< a display plug-in won the screen (else a plain monitor, no DP)
	bool dp_tracks;     //!< that DP advertises eye tracking for this screen
	uint32_t dp_policy; //!< that DP's viewer policy for this screen (absent slot already FIXED)
	//! Desktop rect (the space the OS places windows in) + physical size: the mapping's inputs.
	int32_t desktop_left;
	int32_t desktop_top;
	uint32_t desktop_width;
	uint32_t desktop_height;
	float width_m;
	float height_m;
};

//! The provider side of the per-screen arbitration (one provider for every screen).
struct target_viewer_provider_in
{
	bool hosted;    //!< this process may host providers
	bool usable;    //!< a resident provider claimed a usable source
	const char *id; //!< its registration id (when @ref usable)
	bool enabled;   //!< an enabled provider row is registered (for "standby")
};

//! The answer for one screen.
struct target_viewer_screen_decision
{
	uint64_t screen_id;
	bool is_primary;
	struct target_viewer_decision d;
	/*!
	 * SOURCE_PROVIDER: what to ADD to a provider sample (primary display
	 * space) to express it in this screen's display space. (0,0,0) on the
	 * primary.
	 */
	float offset_m[3];
};

/*!
 * Where the centre of @p home lies in @p to's display space, metres: the
 * offset a point in @p home's display space is translated by to land in
 * @p to's. Through desktop pixels with each screen's own pixel pitch (width_m
 * / desktop_width), z = 0 — the workspace's one-layout model (ADR-052 D2,
 * `comp_ws_map_pose`): exact for coplanar screens, an approximation for
 * angled ones. A screen with no physical size borrows the other's pitch.
 * False when neither has a usable pitch (or a rect is empty).
 */
bool
target_viewer_map_offset(const struct target_viewer_screen_in *home,
                         const struct target_viewer_screen_in *to,
                         float out_offset_m[3]);

/*!
 * The pure per-screen arbitration. Each screen gets @ref target_viewer_arbitrate
 * with ITS DP's inputs and the one @p provider (a screen without a DP gets the
 * nominal viewer: "no display processor on this screen"); a screen the
 * provider serves other than the primary also gets its mapping offset — or,
 * when no offset can be derived, the nominal viewer with the reason. The home
 * of the provider's samples is the screen flagged
 * @ref target_viewer_screen_in::is_primary (the first screen when none is).
 * Writes @p count decisions (at most @ref TARGET_VIEWER_MAX_SCREENS) and
 * returns how many.
 */
uint32_t
target_viewer_arbitrate_screens(const struct target_viewer_screen_in *screens,
                                uint32_t count,
                                const struct target_viewer_provider_in *provider,
                                struct target_viewer_screen_decision *out);

/*!
 * What a viewer-tracking provider is doing for a screen, for status
 * (`status --json` `eye_tracking.viewer_provider`). Stable names.
 */
enum target_viewer_provider_state
{
	//! "none": no provider is involved (FIXED, no DP, OWN / tracking display with no provider at all).
	TARGET_VIEWER_PROVIDER_NONE = 0,
	//! "serving": the views follow it and it is producing.
	TARGET_VIEWER_PROVIDER_SERVING,
	//! "idle": chosen, not started yet (it starts on the first viewer request).
	TARGET_VIEWER_PROVIDER_IDLE,
	//! "paused": chosen and started, but its source is gone or its samples are stale (views: last known).
	TARGET_VIEWER_PROVIDER_PAUSED,
	//! "standby": usable, outranked — the display tracks (or owns) the viewer on this screen.
	TARGET_VIEWER_PROVIDER_STANDBY,
	//! "no_source": the screen would take one (EXTERNAL_OK), none is usable.
	TARGET_VIEWER_PROVIDER_NO_SOURCE,
};

//! The stable spelling of @p s.
const char *
target_viewer_provider_state_str(enum target_viewer_provider_state s);

/*!
 * The pure state rule: @p d the screen's decision, @p started / @p producing
 * the chosen provider's live facts (started at all; its source is producing
 * fresh samples).
 */
enum target_viewer_provider_state
target_viewer_provider_state_of(const struct target_viewer_decision *d, bool started, bool producing);


/*
 *
 * Glue.
 *
 */

/*!
 * The service calls this once, before creating its instance. The service
 * HOSTS the provider (one for every IPC client, see the file comment); the
 * flag only changes how the decision is logged.
 */
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

/*!
 * Build the per-screen inputs for @p list: each screen's own DP — the plug-in
 * that won the monitor in @p reg (`owning_iface` / `owning_instance`), its
 * `get_viewer_policy` for that monitor's descriptor, whether the screen info
 * advertises tracking. Returns the count written (at most @p max).
 */
uint32_t
target_viewer_screens_from_list(const struct xrt_screen_list *list,
                                const struct xrt_dp_factory_registry *reg,
                                struct target_viewer_screen_in *out,
                                uint32_t max);

/*!
 * Re-run the per-screen arbitration for a newly published display topology
 * (system create, hotplug, re-probe — `target_instance.c`) and record it for
 * @ref xrt_viewer_tracking. The primary screen keeps the decision the apply
 * recorded (static per instance: the advertisement was composed from it).
 * Logs one WARN per screen whose answer changed. Runs the lazy provider scan
 * only when some screen can take a provider.
 */
void
target_viewer_tracking_update_screens(const struct xrt_screen_list *list, const struct xrt_dp_factory_registry *reg);

/*!
 * The per-screen answers for @p list, for status: the recorded ones (this
 * process arbitrated — in-process runtime, service), else evaluated now
 * without starting anything (a headless CLI). Writes one entry per screen of
 * @p list, in its order (at most @p max), and returns the count.
 */
uint32_t
target_viewer_tracking_screen_decisions(const struct xrt_screen_list *list,
                                        const struct xrt_dp_factory_registry *reg,
                                        struct target_viewer_screen_decision *out,
                                        uint32_t max);

//! The live provider state for one screen's decision (reads the chosen provider's status and ring).
enum target_viewer_provider_state
target_viewer_tracking_provider_state(const struct target_viewer_decision *d);

//! The interface for @ref xrt_system_devices::viewer_tracking. Never NULL.
struct xrt_viewer_tracking *
target_viewer_tracking_get_iface(void);

//! Is the chosen provider started, and what does it say about itself (`get_status`)? For status.
bool
target_viewer_tracking_provider_running(uint32_t *out_status);

/*!
 * The service calls this once, before creating its instance, with the
 * absolute path of the sibling `displayxr-tracking-host` (NULL / "" = none:
 * the provider is then hosted in the service, still writing the page). Only
 * the service hosts out of process (tracking-provider-discovery.md §4a).
 */
void
target_viewer_tracking_set_host_exe(const char *path);

//! Where the provider runs, for status (`status --json` per screen `eye_tracking.viewer_hosting` / `host_*`).
struct target_viewer_hosting_info
{
	//! "in_process" (loaded here), "host" (the service's displayxr-tracking-host), "service" (in the
	//! service, no host binary), "page" (this process reads the service's page), "" (not resolved).
	char hosting[16];
	int64_t host_pid;    //!< the host (or hosting service) pid; 0 = none
	char host_state[16]; //!< u_vpp_state_str: "idle" / "starting" / "running" / "backoff" / "failed" / "stopped"
	uint32_t restarts;   //!< host restarts since the service started
};

void
target_viewer_tracking_get_hosting(struct target_viewer_hosting_info *out);

//! Stop the provider (system teardown). A later system starts it again on demand.
void
target_viewer_tracking_shutdown(void);

#ifdef __cplusplus
}
#endif
