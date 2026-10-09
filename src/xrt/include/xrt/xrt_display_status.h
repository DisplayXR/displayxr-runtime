// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vendor-generic display status snapshot (ADR-051 D1).
 *
 * The ONE model every status renderer reads: `displayxr-cli status`, the
 * Control Panel, the MCP surface and any later web page. Its JSON shape is
 * `docs/roadmap/display-dashboard.md` §3 (`u_status_snapshot_to_cjson`), its
 * text shape is §7 (`u_status_snapshot_format_text`), and its runtime-derived
 * warnings are §4 (`u_status_warnings_derive`).
 *
 * Plain data, fixed size, NO pointers: phase 2 serves it from the service over
 * the DIAG IPC path by value (in pieces — see the size note below).
 *
 * Who fills what, by phase:
 *  - phase 1: the headless builder (`target_status_snapshot_build_headless`) —
 *    runtime, plugins, screens (identity, desktop, native, physical, roles,
 *    claim, layout, eye-tracking caps); `source = HEADLESS`, no clients,
 *    generation 0, every screen `eye_tracking.state = NO_DP`, no `mode`, no `dps`.
 *  - phase 2: the service — generation counters, clients[], per-screen mode,
 *    live tracking state and the bound DPs, workspace.
 *  - phase 3: the plug-in slot `get_screen_status` — the per-screen vendor cell.
 *
 * Size: tens of KB (see `tests_status_snapshot`), so the service does not send
 * it as one reply: it crosses as @ref xrt_status_head + one
 * @ref xrt_status_screen per `system_get_status_snapshot(screen_index)` reply
 * + one @ref xrt_status_client per `system_get_client_segments(client_id)`
 * reply — each a fixed-size struct by value, the mechanism
 * `system_enumerate_displays` uses for `xrt_screen_list` (see
 * `u_status_snapshot_get_head` / `u_status_snapshot_set_head`).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_screen.h"
#include "xrt/xrt_display_metrics.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Snapshot schema; bumped only on an incompatible change (keys are only ever added).
#define XRT_STATUS_SCHEMA 1

//! Registered plug-ins carried in a snapshot.
#define XRT_STATUS_MAX_PLUGINS 8
//! Screens carried in a snapshot (= the screen-list max).
#define XRT_STATUS_MAX_SCREENS XRT_SCREEN_LIST_MAX
//! Clients carried in a snapshot (= IPC_MAX_CLIENTS, which xrt/ cannot include).
#define XRT_STATUS_MAX_CLIENTS 32
//! Warnings per screen and at system level.
#define XRT_STATUS_MAX_WARNINGS 8
//! Display processors bound to one screen.
#define XRT_STATUS_MAX_SCREEN_DPS 8
//! Segments per client (= XRT_MAX_SEGMENTS).
#define XRT_STATUS_MAX_SEGMENTS XRT_MAX_SEGMENTS
//! Size of a warning code, incl. the NUL.
#define XRT_STATUS_CODE_MAX 32
//! Size of a warning text, incl. the NUL.
#define XRT_STATUS_TEXT_MAX 128
//! Size of short identity strings (ids, names, serials), incl. the NUL.
#define XRT_STATUS_NAME_MAX 64

/*!
 * Bits for @ref xrt_status_claim::apis — the graphics APIs whose DP factory the
 * winning claim offers on that screen.
 * @{
 */
#define XRT_STATUS_API_BIT_D3D11 (1u << 0)
#define XRT_STATUS_API_BIT_D3D12 (1u << 1)
#define XRT_STATUS_API_BIT_VK (1u << 2)
#define XRT_STATUS_API_BIT_GL (1u << 3)
#define XRT_STATUS_API_BIT_METAL (1u << 4)
/*! @} */

//! Where the snapshot was built (JSON "service" / "headless").
enum xrt_status_source
{
	XRT_STATUS_SOURCE_SERVICE = 0,  //!< Assembled in the running service (live).
	XRT_STATUS_SOURCE_HEADLESS = 1, //!< Assembled in-process: "what a process starting now would get".
};

//! Live tracking state of a screen (JSON upper-case).
enum xrt_status_tracking
{
	XRT_STATUS_TRACKING_TRACKING = 0,     //!< A DP bound to the screen reports a tracked viewer.
	XRT_STATUS_TRACKING_NOT_TRACKING = 1, //!< A DP is bound, nobody is tracked.
	XRT_STATUS_TRACKING_NO_DP = 2,        //!< No DP is bound to the screen (always, headless).
	XRT_STATUS_TRACKING_UNKNOWN = 3,      //!< A DP is bound but did not answer.
};

//! Who presents a client's frames (JSON upper-case).
enum xrt_status_presenter
{
	XRT_STATUS_PRESENTER_APP_HWND = 0,       //!< The service presents into the app's own window.
	XRT_STATUS_PRESENTER_CLIENT_TEXTURE = 1, //!< The client presents a shared texture itself.
	XRT_STATUS_PRESENTER_NONE = 2,           //!< Nothing presents for it right now.
	XRT_STATUS_PRESENTER_SERVICE_WINDOW = 3, //!< Hosted: the service presents into its own window.
	XRT_STATUS_PRESENTER_SELF = 4,           //!< A weave present-owner: it weaves and presents itself.
};

//! Who holds the panel lease, from a client's point of view (JSON lower-case, §3).
enum xrt_status_lease
{
	XRT_STATUS_LEASE_CONTROLLER = 0, //!< A workspace controller holds it.
	XRT_STATUS_LEASE_SLOT = 1,       //!< This client's slot holds it (default policy).
	XRT_STATUS_LEASE_NONE = 2,       //!< Nobody / not this client.
};

//! Warning severity (JSON lower-case, §4). Only WARN and CRITICAL light a badge.
enum xrt_status_level
{
	XRT_STATUS_LEVEL_INFO = 0,
	XRT_STATUS_LEVEL_WARN = 1,
	XRT_STATUS_LEVEL_CRITICAL = 2,
};

//! Vendor tracker state of a screen (phase 3 plug-in slot; JSON upper-case).
enum xrt_status_vendor_tracker
{
	XRT_STATUS_VENDOR_TRACKER_UNKNOWN = 0,     //!< Not reported.
	XRT_STATUS_VENDOR_TRACKER_NONE = 1,        //!< The screen has no tracker.
	XRT_STATUS_VENDOR_TRACKER_OFF = 2,         //!< Present, not running.
	XRT_STATUS_VENDOR_TRACKER_STARTING = 3,    //!< Starting up.
	XRT_STATUS_VENDOR_TRACKER_RUNNING = 4,     //!< Running.
	XRT_STATUS_VENDOR_TRACKER_DOWN = 5,        //!< Expected to run and is not.
	XRT_STATUS_VENDOR_TRACKER_UNSUPPORTED = 6, //!< The platform cannot track this screen.
};

//! Vendor lens state of a screen (phase 3; JSON "2D" / "3D" / "UNKNOWN").
enum xrt_status_vendor_lens
{
	XRT_STATUS_VENDOR_LENS_UNKNOWN = 0,
	XRT_STATUS_VENDOR_LENS_2D = 1,
	XRT_STATUS_VENDOR_LENS_3D = 2,
};

//! Where a screen's millimetres came from (JSON lower-case).
enum xrt_status_mm_source
{
	XRT_STATUS_MM_SOURCE_NONE = 0,   //!< Unknown — the 0 m trap.
	XRT_STATUS_MM_SOURCE_EDID = 1,   //!< The monitor's EDID.
	XRT_STATUS_MM_SOURCE_PLUGIN = 2, //!< The claiming plug-in's display info.
};

//! Graphics API of a bound DP (JSON lower-case).
enum xrt_status_dp_api
{
	XRT_STATUS_DP_API_D3D11 = 0,
	XRT_STATUS_DP_API_D3D12 = 1,
	XRT_STATUS_DP_API_VK = 2,
	XRT_STATUS_DP_API_GL = 3,
	XRT_STATUS_DP_API_METAL = 4,
};

//! Role of a bound DP (JSON lower-case).
enum xrt_status_dp_kind
{
	XRT_STATUS_DP_KIND_PRIMARY = 0, //!< The session's own DP.
	XRT_STATUS_DP_KIND_SEGMENT = 1, //!< A per-segment DP (ADR-047 multi-screen).
};

//! Backend health of a bound DP; values equal `XRT_DP_BACKEND_STATE_*` (JSON upper-case).
enum xrt_status_dp_backend
{
	XRT_STATUS_DP_BACKEND_OK = 0,
	XRT_STATUS_DP_BACKEND_DEGRADED = 1, //!< Reconnecting to its vendor backend.
	XRT_STATUS_DP_BACKEND_STALE = 2,    //!< Cannot reconnect; being recreated.
};

//! Where a segment's eyes come from (JSON upper-case).
enum xrt_status_eye_source
{
	XRT_STATUS_EYE_SOURCE_NONE = 0,    //!< No eyes (flat, or the DP has none).
	XRT_STATUS_EYE_SOURCE_DP = 1,      //!< That screen's own DP.
	XRT_STATUS_EYE_SOURCE_PRIMARY = 2, //!< Borrowed from the session's primary DP.
};

/*!
 * One warning: `{code, level, text}` (§4). Runtime-derived codes are listed in
 * `u_status_snapshot.h`; vendor codes pass through verbatim in the vendor cell.
 */
struct xrt_status_warning
{
	char code[XRT_STATUS_CODE_MAX]; //!< Stable upper-case code, e.g. "NOT_NATIVE".
	enum xrt_status_level level;    //!< Severity.
	char text[XRT_STATUS_TEXT_MAX]; //!< One sentence that says what to do.
};

/*!
 * The vendor summary cell of one screen (ADR-051 D2). Phase 3 fills it from the
 * plug-in slot `get_screen_status`; until then @ref present is false everywhere.
 */
struct xrt_status_vendor
{
	bool present;                            //!< The claiming plug-in answered (false = "no vendor status").
	bool ready;                              //!< The vendor says the screen is usable.
	bool verified;                           //!< The vendor verified the screen's identity.
	bool calibrated;                         //!< The vendor has a calibration for it.
	enum xrt_status_vendor_tracker tracker;  //!< Tracker state.
	enum xrt_status_vendor_lens lens;        //!< Lens state.
	char model[32];                          //!< Vendor model string, shown verbatim.
	char serial[32];                         //!< Vendor serial, shown verbatim.
	struct xrt_status_warning worst_warning; //!< Worst vendor warning; empty code = none.
	char dashboard_command[160];             //!< Launch template ("{serial}" / "{monitor_id}"); "" = none.
};

/*!
 * One registered display plug-in (ADR-045 record + registration).
 */
struct xrt_status_plugin
{
	char id[XRT_STATUS_NAME_MAX];      //!< Discovery id.
	char name[128];                    //!< Registration DisplayName.
	char vendor[XRT_STATUS_NAME_MAX];  //!< Registration vendor ("" if unknown).
	char version[XRT_STATUS_NAME_MAX]; //!< Registration Version.
	char load[24];                     //!< Loader outcome, e.g. "ACTIVE", "DECLINED", "NOT_ATTEMPTED".
	uint32_t platform_state;           //!< `enum xrt_plugin_platform_state` (0 = UNKNOWN).
	char hint[128];                    //!< The plug-in's platform hint, verbatim.
	bool fallback;                     //!< A FALLBACK plug-in (sim-display).
	bool active;                       //!< The active plug-in of the process that built the snapshot.
	uint32_t probe_order;              //!< ProbeOrder (lower wins).
};

//! EDID identity of a screen.
struct xrt_status_edid
{
	char manufacturer[4]; //!< 3-letter PNP code, e.g. "AUO"; "" when unknown.
	char product[8];      //!< Product code as 4 hex digits; "" when unknown.
	uint32_t serial;      //!< EDID serial number; 0 = none.
};

//! A screen's place on the desktop.
struct xrt_status_desktop
{
	int32_t left;   //!< Desktop rect, the space the OS places windows in.
	int32_t top;    //!< See @ref left.
	uint32_t width; //!< Current mode width, pixels.
	uint32_t height;
	float scale; //!< Desktop compositor scale (e.g. 2.5); 0 = unknown.
};

//! A screen's native mode.
struct xrt_status_native
{
	uint32_t width;       //!< Native width, pixels; 0 = unknown.
	uint32_t height;      //!< Native height, pixels; 0 = unknown.
	uint32_t refresh_mhz; //!< Current refresh, milli-Hz; 0 = unknown.
	bool is_native;       //!< The desktop mode equals the native mode (false when unknown).
};

//! A screen's physical size.
struct xrt_status_physical
{
	uint32_t width_mm;                //!< Visible width, mm; 0 = unknown.
	uint32_t height_mm;               //!< Visible height, mm; 0 = unknown.
	enum xrt_status_mm_source source; //!< Where the numbers came from.
};

//! The three distinct "primary" notions (ADR-051 D1).
struct xrt_status_roles
{
	bool os_main;         //!< The OS desktop's primary monitor.
	bool runtime_default; //!< The runtime's SYSTEM_DEFAULT screen (where a non-spanning window lands).
	bool vendor_primary;  //!< The vendor's primary (phase 3; false until then).
};

//! The winning display claim of a screen.
struct xrt_status_claim
{
	char plugin_id[XRT_STATUS_NAME_MAX]; //!< Winning plug-in ("" = unclaimed).
	uint32_t confidence;                 //!< `enum xrt_display_claim_confidence` value (0 = none).
	char serial[XRT_STATUS_NAME_MAX];    //!< Vendor serial from the claim ("" if n/a).
	uint32_t apis;                       //!< `XRT_STATUS_API_BIT_*` factories the claim offers.
};

//! The metres + nominal viewer the segment layout uses for a screen.
struct xrt_status_layout
{
	float width_m;            //!< Physical width, metres; 0 = unknown.
	float height_m;           //!< Physical height, metres; 0 = unknown.
	float nominal_viewer_x_m; //!< Nominal viewer, screen-centre relative, metres.
	float nominal_viewer_y_m; //!< See @ref nominal_viewer_x_m.
	float nominal_viewer_z_m; //!< See @ref nominal_viewer_x_m.
	uint32_t source;          //!< `enum xrt_screen_info_source`.
};

//! Eye-tracking capability and live state of a screen.
struct xrt_status_eye_tracking
{
	uint32_t supported;             //!< Mode bits (bit 0 MANAGED, bit 1 MANUAL); 0 = none.
	uint32_t default_mode;          //!< 0 MANAGED, 1 MANUAL (meaningful iff @ref supported).
	enum xrt_status_tracking state; //!< Live state (phase 2; NO_DP headless).
	uint32_t not_tracking_ms;       //!< How long @ref state has been NOT_TRACKING (phase 2; feeds TRACKER_DOWN).
};

//! The rendering mode a screen is in (phase 2; absent headless).
struct xrt_status_mode
{
	bool valid;     //!< A DP bound to the screen reported a mode.
	uint32_t index; //!< Rendering-mode index.
	char name[32];  //!< Mode name.
	uint32_t views; //!< View count.
	bool is_3d;     //!< Hardware display in 3D.
};

//! One DP bound to a screen (phase 2).
struct xrt_status_dp
{
	uint32_t client_id;                 //!< IPC client id (0 = the service's own).
	enum xrt_status_dp_api api;         //!< Graphics API of the DP.
	enum xrt_status_dp_kind kind;       //!< Primary or segment DP.
	enum xrt_status_dp_backend backend; //!< Backend health.
};

/*!
 * One screen row — the backbone of the dashboard. Registry order, the system
 * default first.
 */
struct xrt_status_screen
{
	uint64_t id;                                 //!< Registry monitor id (never 0).
	uint32_t index;                              //!< Row index.
	char device_name[XRT_STATUS_NAME_MAX];       //!< OS device name (`\\.\DISPLAY1`, `HDMI-1`); "" = unknown.
	char friendly_name[XRT_STATUS_NAME_MAX];     //!< Human name (EDID model, else "PNP PROD").
	struct xrt_status_edid edid;                 //!< EDID identity.
	struct xrt_status_desktop desktop;           //!< Desktop placement + scale.
	struct xrt_status_native native;             //!< Native mode.
	struct xrt_status_physical physical;         //!< Physical size.
	struct xrt_status_roles roles;               //!< OS main / runtime default / vendor primary.
	struct xrt_status_claim claim;               //!< Winning claim.
	struct xrt_status_layout layout;             //!< Metres + nominal viewer.
	struct xrt_status_eye_tracking eye_tracking; //!< Caps + live state.
	struct xrt_status_mode mode;                 //!< Current rendering mode (phase 2).
	uint32_t dp_count;                           //!< Valid entries in @ref dps.
	struct xrt_status_dp dps[XRT_STATUS_MAX_SCREEN_DPS];         //!< DPs bound to the screen (phase 2).
	struct xrt_status_vendor vendor;                             //!< Vendor cell (phase 3).
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< Derived per-screen warnings (§4).
};

//! A rectangle in window pixels.
struct xrt_status_rect
{
	int32_t x;  //!< Left.
	int32_t y;  //!< Top.
	uint32_t w; //!< Width.
	uint32_t h; //!< Height.
};

//! One segment of a client's window (phase 2).
struct xrt_status_segment
{
	uint64_t screen;                       //!< Screen id the segment lies on.
	struct xrt_status_rect canvas;         //!< Segment rect in the window's canvas, pixels.
	bool has_dp;                           //!< A DP exists for that screen right now.
	bool woven;                            //!< Woven in 3D (else a flat 2D copy).
	enum xrt_status_eye_source eye_source; //!< Where the segment's eyes come from.
};

//! A client's segment table (phase 2).
struct xrt_status_segments
{
	uint64_t generation;                                      //!< `xrt_segment_metrics::generation`.
	bool split;                                               //!< The window is woven per screen.
	uint32_t count;                                           //!< Valid entries in @ref items.
	struct xrt_status_segment items[XRT_STATUS_MAX_SEGMENTS]; //!< Left to right.
};

//! A client's session flags (phase 2).
struct xrt_status_client_flags
{
	bool active;  //!< Session active.
	bool visible; //!< Session visible.
	bool focused; //!< Session focused.
	bool overlay; //!< Overlay session.
};

//! A client's window rect (phase 2).
struct xrt_status_window
{
	bool valid;      //!< The service knows the window.
	int32_t left;    //!< Client-area origin, desktop coordinates.
	int32_t top;     //!< See @ref left.
	uint32_t width;  //!< Client-area size, pixels.
	uint32_t height; //!< See @ref width.
};

//! View-set counts of a client (phase 2).
struct xrt_status_views
{
	uint32_t capacity; //!< Views the swapchain was sized for.
	uint32_t active;   //!< Views the current mode uses.
	uint32_t reported; //!< Views reported to the app (incl. segment view sets).
};

//! Paint / present / skip integrity triple (#1248) — never a rate without it.
struct xrt_status_integrity
{
	uint64_t paint;           //!< Frames painted.
	uint64_t present;         //!< Frames presented.
	uint64_t skip;            //!< Frames skipped.
	char weave_placement[16]; //!< Weave placement verdict, e.g. "scanout".
};

/*!
 * One IPC client (phase 2; empty headless).
 */
struct xrt_status_client
{
	uint32_t id;                           //!< IPC client id.
	int64_t pid;                           //!< Process id.
	uint32_t client_class;                 //!< `enum xrt_client_class` (verified).
	bool class_verified;                   //!< false = still "unverified".
	char name[XRT_STATUS_NAME_MAX];        //!< Application name.
	struct xrt_status_client_flags flags;  //!< Session flags.
	enum xrt_status_presenter presenter;   //!< Who presents.
	enum xrt_status_lease lease;           //!< Panel lease from this client's view.
	struct xrt_status_window window;       //!< Window rect.
	uint64_t owner_screen;                 //!< Screen owning the window handle (0 = none).
	struct xrt_status_segments segments;   //!< Segment table.
	struct xrt_status_views views;         //!< View-set counts.
	struct xrt_status_integrity integrity; //!< Integrity triple + weave placement.
};

//! Runtime identity block.
struct xrt_status_runtime
{
	char version[32];                //!< "MAJOR.MINOR.PATCH".
	char git_tag[128];               //!< Build git tag.
	uint32_t plugin_abi;             //!< XRT_PLUGIN_API_VERSION_CURRENT.
	char active_openxr_runtime[260]; //!< The OS's ActiveRuntime manifest ("" = unset / n/a).
};

//! Generation counters (phase 2; 0 headless).
struct xrt_status_generation
{
	uint64_t topology; //!< Screens + plug-ins.
	uint64_t status;   //!< Everything else.
};

//! Workspace block (phase 2).
struct xrt_status_workspace
{
	bool enabled;                         //!< A workspace session is active.
	char controller[XRT_STATUS_NAME_MAX]; //!< Its controller ("" = none).
};

/*!
 * The snapshot (ADR-051 D1).
 */
struct xrt_status_snapshot
{
	uint32_t schema;                                             //!< @ref XRT_STATUS_SCHEMA.
	enum xrt_status_source source;                               //!< Where it was built.
	struct xrt_status_generation generation;                     //!< Generation counters.
	struct xrt_status_runtime runtime;                           //!< Runtime identity.
	uint32_t plugin_count;                                       //!< Valid entries in @ref plugins.
	struct xrt_status_plugin plugins[XRT_STATUS_MAX_PLUGINS];    //!< Registered plug-ins, by ProbeOrder.
	uint32_t screen_count;                                       //!< Valid entries in @ref screens.
	struct xrt_status_screen screens[XRT_STATUS_MAX_SCREENS];    //!< Screens, registry order, default first.
	uint32_t client_count;                                       //!< Valid entries in @ref clients.
	struct xrt_status_client clients[XRT_STATUS_MAX_CLIENTS];    //!< IPC clients (phase 2).
	struct xrt_status_workspace workspace;                       //!< Workspace state (phase 2).
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< System-level warnings.
};

/*!
 * The snapshot minus its per-screen and per-client rows — the piece every
 * `system_get_status_snapshot` reply carries (ADR-051 D3, phase 2). The rows
 * cross separately, by index (screens) and by id (@ref client_ids); a reader
 * reassembles them with `u_status_snapshot_set_head` and checks that every
 * piece carries the same @ref generation.
 */
struct xrt_status_head
{
	uint32_t schema;                                             //!< @ref XRT_STATUS_SCHEMA.
	enum xrt_status_source source;                               //!< Where it was built.
	struct xrt_status_generation generation;                     //!< Generation the pieces belong to.
	struct xrt_status_runtime runtime;                           //!< Runtime identity.
	uint32_t plugin_count;                                       //!< Valid entries in @ref plugins.
	struct xrt_status_plugin plugins[XRT_STATUS_MAX_PLUGINS];    //!< Registered plug-ins.
	uint32_t screen_count;                                       //!< Screens to fetch by index.
	uint32_t client_count;                                       //!< Valid entries in @ref client_ids.
	uint32_t client_ids[XRT_STATUS_MAX_CLIENTS];                 //!< Clients, snapshot order.
	struct xrt_status_workspace workspace;                       //!< Workspace state.
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< System-level warnings.
};

//! Live DPs the service reports into one build (one primary per client + its segment DPs + its own).
#define XRT_STATUS_MAX_LIVE_DPS 64

/*!
 * One display processor the service has bound right now, as the service
 * builder (`target_status_snapshot_build_service`) maps it onto a screen.
 */
struct xrt_status_live_dp
{
	//! Screen the DP weaves (0 = the system-default screen, which the builder resolves).
	uint64_t screen_id;
	struct xrt_status_dp dp;  //!< Client, API, kind, backend.
	bool answered;            //!< The DP answered the eye query.
	bool is_tracking;         //!< It reports a tracked viewer (iff @ref answered).
	uint32_t not_tracking_ms; //!< How long it has answered NOT tracking (0 when tracking).
};

/*!
 * What only the running service knows (ADR-051 D3), gathered by the IPC
 * server and merged by `target_status_snapshot_build_service` into the rows
 * the headless builder already fills. Plain data, no pointers.
 */
struct xrt_status_live
{
	struct xrt_status_generation generation;                  //!< The service's counters.
	uint32_t client_count;                                    //!< Valid entries in @ref clients.
	struct xrt_status_client clients[XRT_STATUS_MAX_CLIENTS]; //!< Fully filled client rows.
	uint32_t dp_count;                                        //!< Valid entries in @ref dps.
	struct xrt_status_live_dp dps[XRT_STATUS_MAX_LIVE_DPS];   //!< Bound DPs.
	struct xrt_status_mode mode;                              //!< The head's current rendering mode.
	struct xrt_status_workspace workspace;                    //!< Workspace state.
};

#ifdef __cplusplus
}
#endif
